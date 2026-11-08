# Chapter 10 - Debugging tales

Systems software rarely fails with a helpful message. This chapter tells thirteen real bugs from this project: the symptom, the
reasoning that narrowed it down, and the fix. Read it as a collection of *techniques*, not just of facts.

Common to all: **form a hypothesis, find the cheapest experiment that could disprove it, run it, and write down what you saw.**

---

## Tale 1 - "unrecognized command-line option"

**Symptom.** The first attempt to build a trivial "hello" module on the board stopped at once:

```
gcc: error: unrecognized command-line option '-mstack-protector-guard=tls'
```

**Reasoning.** The option is not in our Makefile, so it comes from the kernel's build system, which copies the flags used to build the kernel itself.
`/proc/config.gz` showed `CONFIG_CC_VERSION_TEXT="arm-xilinx-linux-gnueabi-gcc (GCC) 12.2.0"`, while `gcc --version` on the board said 11.2.0. Older compilers do not know options newer ones added.
Suppressing the options one at a time fails on the next one (`-ftrivial-auto-var-init=zero`).

**Fix.** Install a compiler of the same major version (`gcc-12` from the Ubuntu archive) and build with `CC=gcc-12`.

**Lesson.** A tool's *configuration* can encode assumptions about another tool's version. Compare versions before you compare code.

---

## Tale 2 - the missing header

**Symptom.** With the right compiler the build then stopped with `fatal error: asm/rwonce.h: No such file or directory`.

**Reasoning.** The file does not exist in any source tree: it is *generated* during preparation (`make modules_prepare`) from a template. The board's header tree had been
trimmed after the kernel build and lacked the `arch/arm/include/generated/` directory. Hypothesis: the tree is incomplete; test: `ls arch/arm/include/generated` -> "No such file".

**Fix.** Copy the tree somewhere writable, generate the missing files there (our first attempt used a plain `make modules_prepare`, which looped forever; Tale 8 has the story and the one-line fix), build against the copy. Doing this in a private directory, not in
`/usr`, leaves the system's files untouched, which also makes the step easy to undo.

**Lesson.** Distributions ship "headers" that are sometimes only *almost* a build tree. Know which files are generated.

---

## Tale 3 - modetest crashes, but not because of us

**Symptom.** After the driver loaded, `modetest` (the standard DRM test tool) crashed with a segmentation fault in `strdup()`. Was the new driver returning bad data?

**Reasoning.** `strace` showed the tool opening `/dev/dri/card0` (not ours) and crashing right after the first `DRM_IOCTL_VERSION` calls. `card0` belongs to the Xilinx accelerator (`zocl`). `gdb` gave the
same location. We then wrote our own probe (`tests/kms_probe.c`), pointed it only at `card1`, and it worked.

**Fix.** Use a test that touches only our device; later (Tale 6) the same crash reappears in Xorg and is routed around.

**Lesson.** When a standard tool fails, find *which object* it was holding when it failed. Then test the smaller thing in isolation: our own 130-line client was worth more than any amount of staring.

---

## Tale 4 - libdrm needs an OF node

**Symptom.** Even on our own device, `modetest` crashed the same way until the driver's platform device had a devicetree node.

**Reasoning.** `/sys/class/drm/card1/device/uevent` for our device had no `OF_COMPATIBLE_0=` line; for `zocl` it did. libdrm parses platform devices by reading those lines. A comparison of
the "good" device and the "bad" one (compare two things that should look alike!) showed the difference.

**Fix.** Create a one-node devicetree with an *OF changeset* when the module loads.

**Gotcha inside the fix.** The first version also called `platform_device_add()` and failed with `sysfs: cannot create duplicate filename '/devices/platform/panther-drm'`. Adding the node
makes the kernel create the platform device *by itself* through a notifier; our own device then clashed. The call trace in `dmesg` showed `platform_device_add` and the name `panther-drm`. Read kernel warnings top to bottom - the message and call trace say precisely which two things collided. The fix was to delete our manual device and let the kernel create it.

---

## Tale 5 - the first mode set works, the page flip hangs

**Symptom.** `kms_probe` set the 1080p mode, then failed with `timeout waiting for a flip event`, with the kernel log showing `flip_done timed out`. But running it a *second* time worked.

**Reasoning.**
1. `echo 0x36 > /sys/module/drm/parameters/debug` turned on KMS, atomic and vblank logging.
2. The log from the failing run contained *no* `updating vblank count` lines at all; the working run had one every 33 ms. So the vblank machinery never started on the first run.
3. In the log, the driver's `update` ran *before* `enable` on the very first commit, so no vblank had been turned on yet when the first flip event was queued.
4. A look at the helper's source/header showed that the *simple display pipe does not call `drm_crtc_vblank_on()` for you*; drivers with their own vblank must.

**Fix.** Call `drm_crtc_vblank_on()` at the end of `enable` and `drm_crtc_vblank_off()` in `disable`, and in `disable` complete any pending event so that user space never waits for something that cannot happen.

**Lesson.** "Works the second time" nearly always means a *state* problem: something the first run leaves behind that the second run depends on. Turn up logging and compare the two logs.

---

## Tale 6 - Xorg segfaults on a device that is not ours

**Symptom.** The whole chain worked up to Xorg, which printed `Platform probe for .../axi:zyxclmm_drm/drm/card0` and then `Segmentation fault at address 0x0`.

**Reasoning.** This was the Tale 3 crash again, now inside X's device scan. We could not modify X or libdrm, and removing `zocl` would have broken other PYNQ features. Options: (a) hide `card0` from X; (b) fix libdrm; (c) unload `zocl`. X only considers devices on its own *seat*; a udev rule can change a device's seat. We first wrote the rule's path pattern as `*/zyxclmm_drm/*`, which did not match the real path component `axi:zyxclmm_drm`; `udevadm info /sys/class/drm/card0` showed `ID_SEAT` was still unset, which told us the rule did not fire. Correcting the glob to `*zyxclmm_drm*` fixed it.

**Lesson.** Prefer the smallest, most reversible workaround, and *verify that it took effect* with a tool that reports the final state (`udevadm info`), not by assuming the rule matched.

---

## Tale 7 - the guard that guarded the wrong register

**Symptom.** The driver's safety check (`the PL is not configured`) refused to load although the base overlay was running.

**Reasoning.** The first version read `PCFG_DONE` from the `MCTRL` register of the Zynq's configuration block, from memory of the reference manual. Reading the registers *directly from user space* with
a few lines of Python and `/dev/mem` printed: `INT_STS = 0x00020004`, `MCTRL = 0x30800100`. Bit 2 is set in `INT_STS`, clear in `MCTRL`. The kernel's own FPGA driver (`zynq-fpga`) also waits on `INT_STS` bit 2.

**Fix.** Read `INT_STS` (offset `0x0C`), bit 2.

**Lesson.** When memory and reality disagree, *measure*. Raw register reads from user space are a superb debugging tool - in a throw-away script, *where a mistake cannot crash the kernel* (a bad access kills only the script).

---

## Tale 8 - the build that never ended (and the exit status that lied)

**Symptom.** The installer's preparation step ran for 25 minutes. The board still answered, but the user asked whether it had hung. `ps` showed *many* `make -f ./scripts/Makefile.build obj=. prepare` processes, memory down to about 100 MB free, 1,100 "overriding recipe" warnings in the log, and process IDs that had already wrapped past 32,768.

**Reasoning.**
1. Each stuck process was the *child* of the previous one, about seven seconds apart: a chain, not a pile. `MAKELEVEL` in `/proc/<pid>/environ` read 109 and rising: **unbounded recursion**.
2. The warnings said that both `scripts/Makefile.build` and the top-level `Makefile` defined the same rules. That happens only if the build system reads the top-level `Makefile` where it expected the small root `Kbuild` file. `ls Kbuild` -> *No such file*: the trimmed header tree lacks it, so `make` fell back to including the very file that started the recursion.
3. "But it worked the first time!" The first preparation, run by hand, had been piped: `make ... | tail -15`. A pipeline's exit status is the **last** command's, so `tail` reported success while `make` was dying. Re-running the original, supposedly good tree, showed the same recursion. The first run had simply produced *enough* files before running out of steam: the modules built.
4. So which step is useless to us? The recursion is the line `$(Q)$(MAKE) $(build)=. prepare` in the target `prepare0`. It only regenerates headers (`bounds.h`, `asm-offsets.h`) that the image already ships. The line *before* it builds the tool we do need, `modpost`, so we could not simply stop at an earlier target: a first attempt with `make archprepare` ended in six seconds with exit status 0, but a *fresh* tree then lacked `modpost` (the installer's own completeness check caught this).

**Fix.** In the private copy only, delete that one line (the installer first checks that exactly one line matches, and refuses to continue otherwise), then run the normal `make modules_prepare`: 25 seconds, exit status 0, no warnings. A 30-minute `timeout` surrounds it, and the tool's exit status is checked directly (no pipe).

**Lessons.** (a) A process *tree* with growing depth means recursion; look at the parent chain and the `MAKELEVEL`. (b) "It worked last time" is a hypothesis; re-run the old experiment. (c) `set -o pipefail`, or no pipe. (d) Put a time limit on any step that could loop.

---

## Tale 9 - the unit file that reloaded the FPGA

**Symptom.** Whenever the installer reached its last step (starting the services), the board stopped answering. It had to be power-cycled, and it came back normal. It happened three times. At first it looked like a network fault, then a power problem, then the new audio driver.

**Reasoning.**
1. *Make the evidence survive the crash.* The journal was stored in RAM and vanished at every reset. We switched on a persistent journal (`mkdir /var/log/journal`), reproduced the failure, and read the dead boot's log from the next one.
2. *Beware mixing boots.* The first look at the log showed bitstream loads from the previous evening's experiments, because the board has no clock to date them and the journal had accumulated old entries. Selecting the boot by its ID (`journalctl _BOOT_ID=...`) and using real timestamps fixed that. Our first conclusion ("something else is loading overlays") was wrong, and we said so.
3. *Read it line by line.* The last minute of the crashed boot reads:
   ```
   09:39:29 Stopping Buttons, switches and LEDs ...        (the installer restarts panther-io)
   09:39:29 Starting Executing boot.py from the boot partition...
   09:40:02 fpga_manager fpga0: writing base.bin to Xilinx Zynq FPGA Manager
   09:40:08 Starting Executing boot.py from the boot partition...   (again, for panther-audio)
   ```
   Nothing in our code runs `boot.py`. But `bootpy.service` appeared in the log each time one of *our* services started.
4. The cause was one line in each of our unit files: `Wants=bootpy.service`. A `Wants=` dependency means "start that unit too, if it is not active". `bootpy.service` is a one-shot that exits when it is done, so it is *inactive* afterwards, and systemd dutifully ran it again: PYNQ reloaded `base.bit`, resetting the whole PL while our display driver's DMA was running and the video clock was being generated. A bus transaction into a PL that is being rewritten never completes; the CPU waits forever.

**Fix.** Delete the `Wants=` lines; keep `After=bootpy.service`. `After=` only orders units that are *already* being started (at boot, `bootpy` is in the same transaction), it never starts anything. The installer's final step was also rewritten to stop the desktop first and to restart only our units.

**Lessons.** (a) In systemd, *ordering* (`After=`) and *activation* (`Wants=`/`Requires=`) are independent; mixing them up has side effects far from the unit you are editing. (b) Make the failure leave evidence before you theorise about it. (c) When the log mixes sessions, filter by an identifier, not by eye. (d) The three leading theories (network, power, the new driver) were each plausible; only the log separated them. (e) This bug is the one the hardware chapter warned about: anything that touches the PL while it is being reconfigured can freeze the machine.

---

## Tale 10 - the menu entry that pointed nowhere

**Symptom.** Under *Applications -> Science -> Mathematics* the menu had an entry *sympy (Python3)*. Clicking it did nothing: no window, no message.

**Reasoning.**
1. *Reproduce the click from a shell.* The menu file shows what the click runs: `x-terminal-emulator -T "sympy (Python3)" -e python3 /usr/bin/isympy`. A terminal that opens and instantly closes looks, to a user, like "nothing happened". Running the command with the desktop's own environment (`env -i` plus the few variables the session sets) printed the answer at once: `python3: can't open file '/usr/bin/isympy': No such file or directory`.
2. *Find out whose fault it is.* `dpkg -L python3-sympy` shows the package ships `isympy.py` in the Python library folder but no `/usr/bin/isympy`; `dpkg --verify` reports nothing missing, so the image had not lost the file: the package never contained it. The package's own menu entry (`/usr/share/menu/python3-sympy`) names a launcher that Ubuntu puts in a *different* package, `isympy3`. A packaging bug, not a bug in the project and not a damaged image.
3. *Check the cause is the only one.* Running `isympy.py` directly worked (`print(1+1)` gave `2`), so only the launcher was missing. A scan of every absolute path in the whole generated menu found exactly one that does not exist: this one.

**Fix.** The installer adds the tiny `isympy3` package (a few kilobytes, no dependencies of note) whenever `python3-sympy` is present, and ends with a *menu check* that reports any menu command naming a missing file, so the next bug of this kind is announced instead of discovered by a user. Installing `isympy3` made the menu entry open a terminal with the sympy prompt.

**Lessons.** (a) "Nothing happens" usually means "something failed quietly": run the *same command* by hand, in the *same environment*, where the error can be seen. (b) A graphical launcher that closes its terminal on error hides every message; `xterm -hold` keeps the window open. (c) Before blaming your own code, ask whose code it is: `dpkg -S`, `dpkg -L` and `dpkg --verify` answer "is the file part of a package, and is it still there?". (d) When you fix one instance of a bug, write the check that finds all instances.

---

## Tale 11 - the dark theme that stopped at the window frame

**Symptom.** After the pink-and-dark theme was installed, the editor and the window frames were dark, but the file manager's menus, toolbar and file list stayed white.

**Reasoning.**
1. *Which parts changed?* The frame around the window (drawn by fluxbox) was dark and pink, so the window manager's style was fine. Inside the window nothing had changed, so the problem was the program's own toolkit.
2. *Rule out the obvious.* The GTK 3 settings were in place (`~/.config/gtk-3.0/settings.ini` and `gtk.css`), and Geany, a GTK 3 program, followed them. A first suspicion was the way the test had been launched (a different `HOME` under `sudo`); printing `HOME` in the same command showed it was correct.
3. *Ask the program what it is.* `ldd /usr/bin/pcmanfm | grep gtk` printed `libgtk-x11-2.0.so.0`: Ubuntu 22.04 builds this file manager with **GTK 2**, which does not read GTK 3's files at all. The package listing had hinted at it: its library was `libfm-gtk.so.4`, with no `3` in the name.

**Fix.** Add the GTK 2 mechanism, `~/.gtkrc-2.0`, which assigns colours per widget state. The file manager then showed a dark violet list with a pink selection.

**The same session found a second small bug.** The desktop showed a large fluxbox logo. The style had said `background: none`, which makes fluxbox re-apply the last wallpaper it remembers. Looking at how the stock styles do it (`background: flat` plus `background.color`) solved it.

**Lessons.** (a) When part of an interface obeys a setting and part does not, find out *which library each part uses* before changing anything. (b) Version numbers hidden in file names (`.so.4`, `-2.0`) are often the clue. (c) Copy the form of an existing, working example (here, a stock style) rather than guessing at syntax.

---

## Tale 12 - the wallpaper that only appeared after a restart

**Symptom.** The pink wallpaper showed up when the desktop was restarted by hand, but after a **reboot** the desktop came up plain black, with the right colours and no picture.

**Reasoning.**
1. *Is the picture or the setter broken?* Running the setter by hand in the session's own (stripped) environment, `env -i HOME=... DISPLAY=:0 PATH=... fbsetbg -a picture.png`, drew the picture. So the file, the package (`feh`) and the environment were fine.
2. *What differs at boot?* A trace of the helper script (`sh -x fbsetbg`) showed its last step: it **records** the request in `~/.fluxbox/lastwallpaper`. The script's own help text says it skips a request equal to the last one. After a reboot fluxbox asks again for the same picture, finds the same record, and does nothing, while the *root window* (where the picture lives) is new and empty.
3. *Why did a manual restart work?* The manual runs wrote the record in a slightly different form, so the next request looked "different" and was applied. That difference hid the bug until the first real reboot.
4. *Test the hypothesis, don't just trust it.* Deleting the record and restarting fixed one start; the real test was to make the fix permanent and **reboot**.

**Fix.** The session script deletes `~/.fluxbox/lastwallpaper` before it starts fluxbox. After the next reboot the wallpaper was on screen with the panther in its place.

**A second layer, found later.** A cold boot once came up without the picture even with the record removed, while four service restarts in a row had all worked: fluxbox makes its single wallpaper request while the board is still busy with start-up work, and that one request can be lost. The session script now re-applies the *active style's own* wallpaper (it reads `background:` and `background.pixmap:` from the style file) 6 and 26 seconds after fluxbox starts. Two further cold boots showed the picture. A race that only appears under boot-time load cannot be proven away by a few passes; the remedy is to make a single lost request harmless.

**Lessons.** (a) A cache that outlives the thing it describes (a file that persists, a screen that does not) is a classic source of "works the second time, fails after reboot". (b) Test the real start-up path (a cold boot), not a convenient imitation of it. (c) Read the helper script's own comments: the behaviour was documented.

---

## Tale 13 - the guard that guarded the wrong layer

**Symptom.** A guard was added to PYNQ so that its tutorial notebooks could load the base overlay while the desktop ran. It passed every check we gave it - and the first real run froze the board.

**What went wrong, in order.**
1. *The first design was built on a partial reading of the code.* A text search for the function that programs the FPGA found `_xrt_download()` in one file, so the guard wrapped that. The search output had been cut short by `| head` (it listed only the first ten lines, all of them class names), which hid the fact that a *subclass* defines its own `download()`. In the subclass, the real programming (a write to `/sys/class/fpga_manager/fpga0/firmware`) happens **before** `_xrt_download()`, and PYNQ's clock-register writes happen before both.
2. *The tests checked the wrong thing.* They called the wrapped function directly with the hardware call faked out. They proved the decision logic was right (skip for the same design, refuse for another) and said nothing about whether the wrapper was *reached* by a real overlay load.
3. *The real run was the first test that could fail.* It did.

**How it was fixed.**
* **Ask the running program, not the source tree.** `inspect.getsource(EmbeddedDevice.download)` printed the real method and its order of operations: shutdown, flags, firmware write, `set_axi_port_width`, `_xrt_download`, bookkeeping. The qualified name printed by `EmbeddedDevice.download.__qualname__` showed which class actually owned the method.
* **Measure the hazard while it is harmless.** With the desktop *stopped* and the guard told to behave as if it were running (`PANTHER_PYNQ_GUARD_FORCE_ACTIVE=1`), a small tracer counted every PS-clock write, device shutdown and firmware write PYNQ made. The stock call makes 4, 1 and 1; a correctly guarded call must make **0, 0, 0**. After the rewrite it did.
* **Move the hook up the stack** (to the top of `Overlay.download()`, before the clocks) and keep PYNQ's bookkeeping.
* **Only then run the dangerous test for real**, in two forms: plain Python and a Jupyter notebook executed through the real kernel. The desktop and the wallpaper were still there afterwards.

**A second, smaller trap.** The first record of "what the FPGA holds" was a hash of the *data passed to the low-level call*, which is not the bitfile and differs from run to run; the record never matched. Hashing the **bitfile PYNQ was asked to load** gives a stable identity.

**Lessons.** (a) A safety check that is never exercised *through the real path* proves only its own logic. (b) Truncated output (`head`, a log tail, a screenshot crop) hides precisely the line that contradicts your theory: when a result is surprising, look at the whole thing. (c) For a hazard that can crash the machine, build a *harmless way to observe it* (a counter, an emulation switch) before you run it for real. (d) Know which method *actually runs*: `obj.method.__qualname__` is one line and settles it. (e) After a failure, say what the failure showed: here the freeze was evidence that the guard sat too low.

---

## Bonus - a safe way to look at registers

```python
import mmap, os, struct
fd = os.open("/dev/mem", os.O_RDONLY | os.O_SYNC)
def rd(addr):
    page = mmap.mmap(fd, 0x1000, mmap.MAP_SHARED, mmap.PROT_READ, offset=addr & ~0xFFF)
    off = addr & 0xFFF
    return struct.unpack("<I", page[off:off+4])[0]
print(hex(rd(0x43C20010)))        # video timing controller version
```

Run as root, only after the base overlay is loaded. A read of an unmapped address in the PL can still cause a bus error.

## What the tales share

1. **Read the log in full**, including warnings above the error.
2. **Isolate**: smallest test, smallest device, smallest change.
3. **Compare** working with failing runs, and good with bad devices.
4. **Look at the generated state** (`/sys`, `udevadm`, register dumps) instead of assuming the configuration took effect.
5. **Route around what you cannot fix**, minimally and reversibly.
