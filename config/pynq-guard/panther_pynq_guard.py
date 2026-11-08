"""panther_pynq_guard - stop PYNQ notebooks from reprogramming the FPGA underneath the running desktop.

Loaded automatically in every Python of the PYNQ virtual environment (panther_pynq_guard.pth), so the stock PYNQ notebooks work unchanged.

The desktop's drivers (video DMA, timing generator, GPIO polling, audio) keep using blocks inside the FPGA. Loading an overlay makes PYNQ
  1. rewrite the PS clock registers for the PL (Overlay.download),
  2. shut the device down and write the bitstream to /sys/class/fpga_manager/fpga0/firmware (EmbeddedDevice.download),
which reconfigures the whole FPGA. A bus access to a block that is being rewritten never completes and the CPU freezes.
Almost every PYNQ tutorial starts with BaseOverlay("base.bit"): the very design that is already loaded.

What this module does, ONLY while the desktop's hardware drivers are loaded (otherwise PYNQ behaves exactly as stock):
  * the FPGA already holds exactly this design  -> skip all of the above (clocks, shutdown, firmware write); PYNQ's bookkeeping still runs,
                                                   so the overlay object is created from the live hardware (like Overlay(..., download=False))
  * a DIFFERENT design (or a partial bitstream)   -> refuse with an explanation (default), or stop the desktop first (mode "stop")

PANTHER_PYNQ_GUARD = protect (default) | stop | off      (off = stock PYNQ; may freeze the board)
The design the FPGA holds is remembered in /run/panther/pl-loaded as the SHA-1 of the bitfile PYNQ last loaded.
PANTHER_PYNQ_GUARD_FORCE_ACTIVE=1 makes the guard behave as if the desktop were running (for tests with the desktop stopped).
"""
import hashlib
import importlib.abc
import importlib.util
import os
import subprocess
import sys

STATE = "/run/panther/pl-loaded"
MODE = os.environ.get("PANTHER_PYNQ_GUARD", "protect").strip().lower()
_FORCE_ACTIVE = os.environ.get("PANTHER_PYNQ_GUARD_FORCE_ACTIVE") == "1"
# modules and devicetree overlays that exist only while the desktop's hardware drivers are in use
_IN_USE = ("/sys/module/panther_drm", "/sys/module/panther_audio", "/sys/kernel/config/device-tree/overlays/panther-io")


def _say(msg):
    print("[pynq-panther] " + msg, file=sys.stderr)


def _hardware_in_use():
    return _FORCE_ACTIVE or any(os.path.exists(p) for p in _IN_USE)


def _fpga_operating():
    try:
        with open("/sys/class/fpga_manager/fpga0/state") as f:
            return f.read().strip() == "operating"
    except OSError:
        return False


def _sha1_of(path):
    try:
        with open(path, "rb") as f:
            return hashlib.sha1(f.read()).hexdigest()
    except OSError:
        return ""


def _recorded():
    try:
        with open(STATE) as f:
            return f.read().strip()
    except OSError:
        return ""


def _record(sig):
    try:
        os.makedirs(os.path.dirname(STATE), exist_ok=True)
        with open(STATE, "w") as f:
            f.write(sig + "\n")
    except OSError:
        pass


def _decide(bitfile, partial=False):
    """'stock' (not our business), 'skip' (same design, leave the hardware alone) or 'protect' (a different design is being loaded)."""
    if MODE == "off" or not _hardware_in_use():
        return "stock"
    if not partial and _fpga_operating() and _recorded() and _recorded() == _sha1_of(bitfile):
        return "skip"
    return "protect"


def _protect(bitfile):
    """Called for 'protect': either stop the desktop (mode stop) or raise. Returns normally only after the desktop is stopped."""
    if MODE == "stop":
        _say("a different design is being loaded: stopping the desktop first (sudo panther-mode desktop brings it back).")
        subprocess.run(["/usr/local/bin/panther-mode", "pynq"], check=False)
        return
    raise RuntimeError(
        "Loading '%s' would reprogram the FPGA while the desktop is using it, which freezes the board.\n"
        "  -> Free the hardware first:  sudo panther-mode pynq    (stops the desktop, sound card and GPIO devices)\n"
        "     ...run your notebook...   then:  sudo panther-mode desktop\n"
        "  -> or let this guard stop the desktop for you: set PANTHER_PYNQ_GUARD=stop before starting Python.\n"
        "  (This protection comes from pynq-panther; see docs/PYNQ-RESOURCES.md.)" % os.path.basename(str(bitfile)))


def _patch_overlay(module):
    """Overlay.download(): decide BEFORE the PS clock registers are rewritten."""
    cls = module.Overlay
    original = cls.download
    if getattr(original, "_panther_guard", False):
        return

    def download(self, dtbo=None):
        verdict = _decide(self.bitfile_name, getattr(self, "partial", False))
        if verdict == "skip":
            _say("the FPGA already holds this design and the desktop is using it: not reprogramming it. "
                 "(For a fresh reset of the design: sudo panther-mode pynq)")
            from pynq.bitstream import Bitstream
            Bitstream.download(self, self.parser)        # -> device.download(): bookkeeping only (see _patch_device)
            if dtbo:
                self.insert_dtbo(dtbo)
            elif getattr(self, "dtbo", None):
                self.insert_dtbo()
            return None
        if verdict == "protect":
            _protect(self.bitfile_name)
        return original(self, dtbo)
    download._panther_guard = True
    cls.download = download


def _patch_device(module):
    """EmbeddedDevice.download(): the sysfs firmware write that really programs the FPGA; also reached directly by Bitstream(...).download()."""
    cls = module.EmbeddedDevice
    original = cls.__dict__.get("download")
    if original is None or getattr(original, "_panther_guard", False):
        return

    def download(self, bitstream, parser=None):
        verdict = _decide(bitstream.bitfile_name, getattr(bitstream, "partial", False))
        if verdict == "skip":
            # everything PYNQ does AFTER loading, without loading: timestamps, name, IP/GPIO/interrupt dictionaries
            if parser is None:
                from pynq.pl_server.xclbin_parser import XclBin
                parser = XclBin(xclbin_data=module.DEFAULT_XCLBIN)
            from pynq.pl_server.device import Device
            Device.post_download(self, bitstream, parser, self.name)
            return None
        if verdict == "protect":
            _protect(bitstream.bitfile_name)
        result = original(self, bitstream, parser)
        if not getattr(bitstream, "partial", False):
            _record(_sha1_of(bitstream.bitfile_name))
        return result
    download._panther_guard = True
    cls.download = download


class _Finder(importlib.abc.MetaPathFinder):
    """Patch PYNQ right after it is imported (nothing of PYNQ is imported earlier than by the user's own code)."""
    _TARGETS = {"pynq.overlay": _patch_overlay, "pynq.pl_server.embedded_device": _patch_device}

    def find_spec(self, name, path, target=None):
        patcher = self._TARGETS.get(name)
        if patcher is None:
            return None
        sys.meta_path.remove(self)
        try:
            spec = importlib.util.find_spec(name)
        finally:
            sys.meta_path.insert(0, self)
        if spec is None or spec.loader is None:
            return None
        original_exec = spec.loader.exec_module

        def exec_module(module, _orig=original_exec, _patcher=patcher):
            _orig(module)
            try:
                _patcher(module)
            except Exception as exc:               # never break PYNQ because of the guard
                _say("could not install the FPGA guard: %r" % (exc,))
        spec.loader.exec_module = exec_module
        return spec


if MODE != "off" and not any(isinstance(f, _Finder) for f in sys.meta_path):
    sys.meta_path.insert(0, _Finder())
