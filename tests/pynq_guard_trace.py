# usage: pynq_guard_trace.py same|stock   (run with the desktop STOPPED; PANTHER_PYNQ_GUARD_FORCE_ACTIVE=1 emulates "desktop running")
# Counts what PYNQ does to the hardware while it creates an overlay: PS clock writes, device shutdowns, fpga_manager firmware/flags writes.
import builtins, sys, time, os
scenario = sys.argv[1]
counts = {"firmware_writes": 0, "flag_writes": 0, "clock_writes": 0, "shutdown_calls": 0}
real_open = builtins.open
class W:
    def __init__(s, f, kind): s.f=f; s.kind=kind
    def write(s, d): counts[s.kind]+=1; return s.f.write(d)
    def __getattr__(s,n): return getattr(s.f,n)
    def __enter__(s): s.f.__enter__(); return s
    def __exit__(s,*a): return s.f.__exit__(*a)
def traced_open(p, mode="r", *a, **k):
    f = real_open(p, mode, *a, **k)
    if isinstance(p,str) and "w" in mode:
        if p.endswith("fpga_manager/fpga0/firmware"): return W(f,"firmware_writes")
        if p.endswith("fpga_manager/fpga0/flags"): return W(f,"flag_writes")
    return f
builtins.open = traced_open
import pynq.ps as ps
orig_clk = type(ps.Clocks).set_pl_clk
def clk(cls, *a, **k): counts["clock_writes"] += 1; return orig_clk(cls, *a, **k)
type(ps.Clocks).set_pl_clk = clk
from pynq.pl_server.embedded_device import EmbeddedDevice
orig_sd = EmbeddedDevice.shutdown
def sd(self,*a,**k): counts["shutdown_calls"] += 1; return orig_sd(self,*a,**k)
EmbeddedDevice.shutdown = sd

def report(tag): print("   hardware actions during this call:", counts, "<-", tag)
if scenario == "same":
    from pynq.overlays.base import BaseOverlay
    t=time.time(); base = BaseOverlay("base.bit"); print("   BaseOverlay() took %.1f s; is_loaded: %s" % (time.time()-t, base.is_loaded()))
    report("must be all zero for a skip")
    base.leds[0].on(); base.leds[0].off(); print("   LED access works; buttons:", len(base.buttons))
elif scenario == "different":
    from pynq import Overlay
    try:
        Overlay("/tmp/other/other.bit"); print("   NO ERROR (bad)")
    except RuntimeError as e:
        print("   refused:", str(e).splitlines()[0][:90])
    report("must be all zero: nothing was touched")
elif scenario == "stock":
    from pynq.overlays.base import BaseOverlay
    t=time.time(); base = BaseOverlay("base.bit"); print("   BaseOverlay() took %.1f s; programmed for real" % (time.time()-t)); report("expected: shutdown+firmware+clock writes")
print("   recorded design now:", open("/run/panther/pl-loaded").read().strip()[:12] if os.path.exists("/run/panther/pl-loaded") else "(none)")
