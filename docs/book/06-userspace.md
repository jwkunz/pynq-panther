# Chapter 6 - Booting into a desktop

## 6.1 The start-up sequence

When the board powers up, the work is passed along a chain:

```
 ROM boot loader -> U-Boot -> Linux kernel -> systemd (PID 1) -> services
                                                |- bootpy.service    : loads the PYNQ base overlay into the PL
                                                |- jupyter.service   : the PYNQ notebook server
                                                |- panther-io.service      (ours: buttons/LEDs)
                                                '- panther-desktop.service (ours: the HDMI desktop)
```

**systemd** starts and supervises services described in *unit files*. Two directives matter most for us:

* `After=bootpy.service` - start only once `bootpy.service` has finished. `bootpy.service` is a `oneshot` service: systemd considers it finished when its command exits, and that command is the script that loads `base.bit`. So *our* services start after the PL holds the base overlay. This is the rule chapter 1 demanded.
* **No `Wants=` or `Requires=` on `bootpy.service`.** `After=` only *orders* units that are both starting anyway (at boot, `bootpy` is part of the boot transaction). `Wants=` would also *activate* it, and a one-shot that has finished is inactive, so starting or restarting one of our services would run `boot.py` again, reloading `base.bit` into the FPGA underneath a running display. That freezes the board. Chapter 10, Tale 9, is the story of how this was found; the lesson is that *ordering* and *activation* are different things in systemd.

The units are in `config/`.

## 6.2 The design rule: never make things worse

A desktop is a convenience. The board's *original* job (headless PYNQ with Jupyter) must keep working whatever happens to the desktop. The supervisor is designed around that:

1. every step has a time limit;
2. every failure ends in "stay headless", logged with a reason, and the program **exits with success**, so systemd does not count it as a failed boot;
3. the service is not a dependency of anything else (nothing waits for it);
4. a settings file can switch it off: `enabled = false` in `/etc/panther/panther.conf`.

In reliability language this is *fail-safe* behaviour: the failure mode is the old, known-good state.

## 6.3 The Rust supervisor, step by step

`session/src/main.rs` implements `panther-session`. Rust was chosen for the user-space program because the language's ownership rules
make it hard to leak the child processes and file descriptors that a supervisor juggles. (The kernel module is C because this
kernel was built without Rust support.) The program's main function reads like the list of steps:

```rust
fn main() {
    let c = Conf::load(CONF);
    if !c.enabled { log!("disabled ..."); return; }
    if !wait_for_pl(c.pl_timeout) { log!("PL never became operational; staying headless"); return; }
    let card = match load_driver(&c) { Ok(card) => card, Err(e) => { log!("{}; staying headless", e); return; } };
    let conf = write_xorg_conf(&card)?;
    loop { start Xorg; wait for it; start the session as the desktop user; supervise; restart a few times }
}
```

1. **Configuration** - a plain `key = value` file; every key has a default so a missing file is not an error.
2. **Wait for the PL** - polls `/sys/class/fpga_manager/fpga0/state` until it reads `operating`.
3. **Load the driver** - runs `modprobe panther_drm` (falling back to `insmod` of the copy in `/usr/local/lib/panther`), then looks through `/sys/class/drm/card*` for the one whose `uevent` says `DRIVER=panther-drm`. We do not hard-code `card1`: the Xilinx accelerator driver takes `card0`, and numbering can change.
4. **Write the X configuration** - generates `/run/panther/xorg.conf` naming that card (`Option "kmsdev"`), selecting the `modesetting` driver, software rendering and ShadowFB.
5. **Start Xorg** as root on virtual terminal 7 (`:0 vt7 -nolisten tcp`: no network access to the display).
6. **Start the session** - waits for X's socket `/tmp/.X11-unix/X0`, then starts `session.sh` *as the desktop user* with a cleaned environment (`DISPLAY`, `HOME`, `PATH`); dropping privileges is done with `CommandExt::uid()`/`gid()`.
7. **Supervise** - if X dies, the session is killed and X restarted, up to `max_restarts` times; a run longer than two minutes resets the count. After the budget is used up it gives up and the board stays headless.

`session.sh` sets the background colour, switches screen blanking off, opens an `xterm` and `exec`s the window manager, fluxbox.

## 6.4 Why Xorg runs as root

Xorg needs access to the DRM device, the virtual terminal and input devices. The modern way (a seat manager, `logind`, hands these to an
unprivileged X) needs a login session, which a boot-time service does not have. Running Xorg as root is simple and safe *here* because `-nolisten tcp` prevents network
connections and the only client is our own session script. A multi-user system should prefer `logind`.

## 6.5 The seat trick (a hack explained)

This board has *two* DRM devices: ours, and `/dev/dri/card0`, the Xilinx accelerator (`zocl`) driver for PYNQ's XRT software. Xorg's startup scan probes **every** DRM
device and, with the library versions on the image, **crashes with a segmentation fault** on `zocl`, which has no display. We cannot fix the library, and we should not
remove `zocl` (other PYNQ features use it). The workaround is a **udev rule** that moves `card0` to a different *seat*:

```
SUBSYSTEM=="drm", KERNEL=="card[0-9]*", DEVPATH=="*zyxclmm_drm*", ENV{ID_SEAT}="seat-xrt", TAG+="seat-xrt"
```

A *seat* is a group of devices that belong to one user's workplace (display, keyboard, mouse). Xorg only considers devices on its
own seat, `seat0`, so it no longer sees `card0`. A neat illustration that a problem can sometimes be *routed around* instead of fixed.

## 6.6 Memory budget

Measured with the desktop running: the whole system uses about 90-150 MB of the 491 MB available, Xorg about 67 MB. Framebuffers come from the 128 MB CMA pool: one 1080p XRGB frame is 7.9 MiB.

## 6.7 Observing and controlling it

| Task | Command |
|---|---|
| Is it running? | `systemctl status panther-desktop` |
| What did the supervisor decide? | `journalctl -u panther-desktop` |
| Why did X fail? | `cat /run/panther/Xorg.log` |
| Stop / start the desktop | `sudo systemctl stop panther-desktop` / `start` |
| Run headless permanently | `sudo systemctl disable panther-desktop` |
| Switch resolution | `DISPLAY=:0 xrandr --output HDMI-1 --mode 1280x720` |
| List modes | `DISPLAY=:0 xrandr` |

## 6.8 Summary

* systemd ordering (`After=bootpy.service`) is what makes the hardware safe to touch.
* The supervisor is a small state machine with time limits, a restart budget and a fail-safe outcome.
* A udev rule moved an incompatible device out of the way instead of fixing the library.

## 6.9 Sharing the board with stock PYNQ notebooks: the guard

Chapter 1 explained why reprogramming the FPGA under a running driver freezes the machine. The *stock PYNQ tutorials* do exactly that on their first line,
`BaseOverlay("base.bit")`, so a desktop on a PYNQ board needs a plan for them. There were three candidate designs.

| Design | Verdict |
|---|---|
| **Edit every notebook** to pass `download=False` (it works: PYNQ builds the overlay objects from the live hardware) | possible, but there are 77 notebooks, they belong to the user, and the next one written will freeze the board again |
| **Make the drivers survive a reprogramming** | the in-kernel GPIO polling cannot be changed from here, and a bus access that lands inside the reconfiguration window cannot be recovered; it would only reduce the odds |
| **Teach PYNQ itself to skip a redundant reload** | one small, reversible change; every notebook, old and new, benefits. *Chosen.* |

**What the guard does** (`config/pynq-guard/panther_pynq_guard.py`). A `.pth` file in PYNQ's virtual environment imports it at every Python start. It installs an *import hook*: when PYNQ's `overlay` and
`embedded_device` modules are imported it replaces two methods, `Overlay.download()` and `EmbeddedDevice.download()`. The replacement asks three questions, all cheap:

1. Are the desktop's hardware drivers loaded (`/sys/module/panther_drm` and friends)? If not, call the original: stock PYNQ.
2. Does the FPGA hold *exactly* the design being requested? The guard keeps the SHA-1 of the last bitfile PYNQ loaded in `/run/panther/pl-loaded` and compares it with the requested file's SHA-1 (and checks that the FPGA manager reports `operating`).
   If yes: **skip** every hardware step (the PS clock registers, the device shutdown, the write to `/sys/class/fpga_manager/fpga0/firmware`), but still run PYNQ's own *bookkeeping* so that the overlay object is complete.
3. Otherwise a different design is being loaded: raise an error that says what to do (or, if you asked for it, stop the desktop first).

**Where the hook must sit** is the whole difficulty (Tale 13). The clock registers are written *before* the device is told to load anything, so the decision has to be made at the top of `Overlay.download()`, not at the bottom.

**Why the record is a hash of the file, not a flag.** "The FPGA holds the base overlay" would be wrong the moment another design is loaded. The hash identifies *which* design; and the recorded value is only ever written after PYNQ really loaded something, so it cannot claim more than is true.

**Limits, said plainly.** A notebook that loads base.bit while the desktop runs gets the *live* design, not a freshly reset one. Notebooks that drive the HDMI or audio blocks still compete with the desktop for them. And the guard is an import hook into a library we do not own: a future PYNQ that renames these methods would make it a no-op (it then prints a warning rather than breaking PYNQ), which is why `tests/pynq_guard_selftest.sh` exists.
