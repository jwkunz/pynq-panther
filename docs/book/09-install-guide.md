# Chapter 9 - Installing, step by step

> **Before the installer:** the board must already boot the stock PYNQ v3.1 image; for that, follow PYNQ's own [PYNQ-Z2 setup guide](https://pynq.readthedocs.io/en/v3.1/getting_started/pynq_z2_setup.html) and [SD-card page](https://pynq.readthedocs.io/en/v3.1/pynq_sd_card.html).

> The *practical* guide for every network situation (a router, a PC sharing its Wi-Fi, or no network at all) is [`docs/INSTALL.md`](../INSTALL.md). This chapter explains what the installer does and why.

This chapter is the practical guide. First the short version, then what each step of the installer does and why, how to check
that it worked, what to do when it does not, and how to remove it.

## 9.1 The short version

On the board (over SSH or a keyboard and monitor), with a network connection:

```sh
sudo apt-get install -y git          # only if git is missing
git clone https://github.com/jwkunz/pynq-panther.git
cd pynq-panther
./install.sh
```

The first run takes **several minutes** (the time is printed at the end), most of it building tools and preparing the kernel build tree (steps 3 and 4). Later runs are much faster.
When it finishes the HDMI monitor shows a desktop, and the desktop starts by itself at every boot.

> **Run it in a way that survives a lost connection.** A long install over SSH should not die if your laptop sleeps. Use
> `tmux` / `screen`, or: `nohup ./install.sh --yes > install.out 2>&1 &` and read `install.out`. (During development the board's Ethernet link
> dropped for a couple of minutes while it was compiling; the build carried on.)

## 9.2 Before you start: what you need

* PYNQ-Z2 running the **PYNQ v3.1** image (kernel 6.6.10-xilinx-v2024.1). Check: `uname -r` should print `6.6.10-xilinx-v2024.1-g...`. Other kernels may work, but have not been tested.
* About **1 GB of free space** on the SD card (`df -h /`), and a network connection to the Ubuntu package archive.
* A **good power supply** (5 V barrel supply with the board's power jumper on "REG"). A weak supply shows up as random resets or network drops when the CPU is busy - exactly what compiling does.
* A monitor on the **HDMI OUT** port (the one nearer the board's edge on the PYNQ-Z2 - the other is an input), a USB keyboard and mouse.

## 9.3 Options

```
./install.sh [--no-start] [--no-enable] [--no-audio] [--no-apps] [--no-theme] [--hostname NAME | --keep-hostname] [--offline DIR] [--rebuild] [--user NAME] [--uninstall] [-y]
```

| Option | Meaning |
|---|---|
| `--no-start` | install and enable at boot, but do not start the desktop now |
| `--no-enable` | install only; nothing starts at boot (you can start it by hand) |
| `--no-audio` | skip the experimental sound driver |
| `--no-apps` | skip the desktop applications (web browser, Geany, micro) and their menu |
| `--no-theme` | keep the stock colours instead of the pink-and-dark theme (chapter 13) |
| `--hostname NAME` / `--keep-hostname` | the board's network name (default `Pynq-Panther`, announced as `pynq-panther.local`) / leave it unchanged |
| `--offline DIR` | install every package from the bundle in `DIR`; the network is never used (section 9.10) |
| `--rebuild` | rebuild even if the kernel did not change (use after a kernel update) |
| `--user NAME` | the account that runs the desktop (default `xilinx`) |
| `--uninstall` | remove everything the installer put in place |
| `-y` | never ask questions |

## 9.4 What the installer does, step by step

The script (`install.sh`, about 200 lines of shell) is meant to be read. Here is each step and the reason for it.

### Step 0 - get root

Installing drivers and services changes the system, so the script re-runs itself under `sudo` (you type the password once, `xilinx` on a default image), and starts logging to `/var/log/panther-install.log`.
It also finds *which user owns the Rust toolchain* - the Rust compiler lives in the *user's* `~/.cargo`, not in root's - so the Rust build is done as that user.

### Step 1 - check the board

It verifies, in order: the machine is 32-bit ARM; an FPGA manager exists (so it is a Zynq); systemd is present; the kernel has a build directory, DRM support and module support; there is a network (or the tools
are already installed); `cargo` exists. If any check fails it stops with a message saying what is missing rather than failing halfway. **Fail early, say why.**

### Step 2 - install packages

Only what is missing is installed, from the Ubuntu archive or, with `--offline`, from the bundle. The build tools: `gcc-12` and `device-tree-compiler`. Why gcc-12: chapter 4 (the kernel was built with GCC 12; the image's default compiler is 11 and rejects flags the kernel passes).
Why the device-tree compiler: to turn the GPIO description (`config/panther-io.dts`) into a binary overlay. The desktop applications (unless `--no-apps`): the Epiphany web browser, the Geany editor, `micro` and the PCManFM file manager (chapter 12), plus `alsa-utils` for the sound card (`aplay`, `arecord`, `amixer`). These are *optional*: each is installed on its own, and one that cannot be installed (say, because an older offline bundle lacks it) only produces a warning. If the PYNQ image's `python3-sympy` is present, the tiny `isympy3` package is added too, because Ubuntu's `python3-sympy` menu entry points at a launcher that only `isympy3` provides (chapter 10, tale 10). After the files are installed the script checks the desktop menu and reports any entry whose command names a file that does not exist. The script also checks that Xorg, fluxbox, xterm and the X tools are present (the PYNQ image has them) and installs any that are missing.

### Step 3 - prepare the kernel build tree  *(the slow step)*

It copies `/lib/modules/$(uname -r)/build` to `/var/lib/panther/kbuild` and runs `make modules_prepare` there, which compiles the host tools and generates the headers the stock tree lacks. One line of the *copy's* Makefile is removed first, because that line recurses forever when the root `Kbuild` file is missing (chapter 4 and Tale 8). A stamp file records the kernel
version, so the step is skipped on later runs unless the kernel changed or you pass `--rebuild`. The original tree is not modified.

### Step 4 - build

* The kernel modules `panther_drm` (display) and `panther_audio` (sound) are compiled against the prepared tree. The script then checks each `.ko`'s *vermagic* contains the running kernel's version, so a module built for the wrong kernel can never be installed.
* The Rust supervisor `panther-session` is built with `cargo build --release --offline` (no downloads needed: it has no dependencies).
* The GPIO overlay `config/panther-io.dts` is compiled to `panther-io.dtbo`.

### Step 5 - install

| What | Where |
|---|---|
| kernel modules | `/lib/modules/<kernel>/extra/` (then `depmod -a`, so `modprobe` finds them) and `/usr/local/lib/panther/` |
| supervisor | `/usr/local/bin/panther-session` |
| session script, GPIO overlay, loader script | `/usr/local/lib/panther/` |
| settings | `/etc/panther/panther.conf` (an existing file is **kept**) |
| browser launcher | `/usr/local/bin/panther-browser` |
| the theme | `~/.fluxbox/styles/PinkDark` (+ `init`), `~/.config/gtk-3.0/`, `~/.gtkrc-2.0`, Geany's colour scheme, `/usr/local/lib/panther/theme/Xresources`; replaced files are saved as `*.pre-panther` |
| network name | `/etc/hostname` (via `hostnamectl`), the `127.0.1.1` line of `/etc/hosts`, `/etc/avahi/avahi-daemon.conf` (`host-name`, `domain-name=local`) and two service files in `/etc/avahi/services/` |
| desktop menu | `~/.fluxbox/menu` of the desktop user (an existing menu is saved as `menu.pre-panther`; `--uninstall` puts it back) |
| systemd units | `/etc/systemd/system/panther-{desktop,io,audio}.service` |
| udev rule (hides the XRT node from X) | `/etc/udev/rules.d/72-panther-xrt-seat.rules` |

Nothing outside the packages above, `/usr/local`, `/etc/panther`, `/etc/systemd/system`, `/etc/udev/rules.d` and the kernel's `extra` folder is written. That is what makes `--uninstall` complete.

### Step 6 - enable and start

`systemctl enable` makes the services start at boot; the script then starts them now. Because the display service is ordered `After=bootpy.service`, a boot starts the desktop only once PYNQ's base overlay is loaded - normally a minute or two after power-on.

## 9.5 Checking that it worked

1. **A desktop** should be on the monitor: a dark background, a terminal window, and a working mouse pointer.
2. **The services:**
   ```sh
   systemctl is-active panther-desktop panther-io panther-audio
   journalctl -u panther-desktop --no-pager | grep panther-session
   ```
   You should see: `waiting for the PL`, `display driver ready on /dev/dri/card1`, `X is up; starting the session as xilinx`.
3. **The graphics device:** `ls /dev/dri` shows `card0` (the Xilinx accelerator) and **`card1`** (ours). `DISPLAY=:0 xrandr` lists `HDMI-1 connected` and the modes.
4. **Buttons and LEDs:** `ls /sys/class/leds | grep pynqz2`; `echo 1 | sudo tee /sys/class/leds/pynqz2:green:ld0/brightness`.
5. **Sound (if installed):** `aplay -l` lists a `PYNQ-Z2 audio` card.
6. **PYNQ still works:** open `http://pynq-panther.local:9090` (Jupyter, password `xilinx`).
7. **Reboot test:** `sudo reboot`; after about two minutes the desktop appears without any command.

## 9.6 Troubleshooting

| Symptom | Likely cause and what to do |
|---|---|
| **Black screen** | Most often X is not running. `systemctl status panther-desktop`, then `journalctl -u panther-desktop`. If it says *staying headless*, the log line before it tells you the reason. Check the cable is in **HDMI OUT**. |
| `install.sh`: "no kernel build directory" | The image has no `/lib/modules/$(uname -r)/build`. Install the matching headers package or use the PYNQ image the project was tested on. |
| `install.sh`: step 3 or 4 seems stuck | The board is slow (650 MHz) and the compile prints nothing. Check progress with `top` in a second session. The preparation step has a 30-minute time limit and the log `/var/log/panther-install.log.kbuild-warnings` holds its messages. If memory fills up and many `make` processes appear (`pgrep -c make` in the hundreds), stop it and report it: that is the runaway of Tale 8. |
| Build error mentioning `-mstack-protector-guard` | Step 2 did not install `gcc-12`, or the module Makefile is not using it (`CC = gcc-12`). Run `gcc-12 --version`. |
| Module will not load: `Invalid module format` | Built for another kernel. Run `./install.sh --rebuild`. After a kernel update this is expected. |
| Module will not load: `No such device` | The PL is not configured yet (the base overlay has not loaded). `cat /sys/class/fpga_manager/fpga0/state` must say `operating`. Wait for boot to finish; look at `systemctl status bootpy`. |
| X exits at once: "Segmentation fault" in `Xorg.log` after `Platform probe ... zyxclmm_drm` | The udev rule is missing. Check `udevadm info /sys/class/drm/card0 \| grep ID_SEAT` says `seat-xrt`. Reinstall or `sudo udevadm trigger --subsystem-match=drm --action=change`. |
| Desktop appears but no mouse/keyboard | `cat /proc/bus/input/devices`; try another USB port or a powered hub; check `dmesg` for USB errors. A weak supply can make the USB port drop devices. |
| Colours look wrong (red and blue swapped) | Please report it: it is an unverified part (the colour-converter matrix, chapter 5). |
| Board resets, freezes or drops off the network | Two known causes. (1) *Reprogramming the FPGA while the desktop runs*: anything that reloads a bitstream (re-running `boot.py`, loading another overlay from a notebook, another project's tests) resets the display hardware; stop the desktop first (`sudo systemctl stop panther-desktop`). (2) A weak power supply under CPU load: use the 5 V barrel supply, not USB. |
| After a PYNQ notebook loads another overlay, the screen goes blank | Expected: loading a bitstream resets the display hardware. Re-run `sudo systemctl restart panther-desktop` after reloading `base.bit`. |

## 9.7 Updating and removing

* **Update the project:** `cd pynq-panther && git pull && ./install.sh` (fast: the kernel tree is already prepared).
* **After a kernel update:** `./install.sh --rebuild`.
* **Turn the desktop off but keep everything:** edit `/etc/panther/panther.conf`, set `enabled = false`.
* **Remove everything:** `./install.sh --uninstall`. It disables and removes the services, modules, supervisor, udev rule and `/usr/local/lib/panther`. It deliberately keeps `/etc/panther/` (your settings), the kernel header copy and installed packages, and prints that it did.

## 9.8 Running the pieces by hand (for learning)

You can start every part manually and watch what happens, which is the best way to understand the boot sequence:

```sh
sudo systemctl stop panther-desktop                  # nothing running now
sudo rmmod panther_drm 2>/dev/null
sudo modprobe panther_drm                            # dmesg: "HDMI output registered"
ls /dev/dri                                          # card1 appears
sudo ./tests/kms_probe /dev/dri/card1 5              # colour bars + page flips, no X needed
sudo /usr/local/bin/panther-session                  # the supervisor, in the foreground, logging to the terminal
```

## 9.9 The desktop menu

Right-click the desktop. At the top is a *Pynq-Z2* menu: **Terminal**, **File manager**, **Web browser**, **Jupyter notebooks** (the board's own notebook server in the browser),
**Text editor - Geany**, **Terminal editor - micro**, a **Display resolution** submenu (the seven modes of chapter 5) and a **Board** submenu (service log, `top`, the sound mixer).
Below a separator is Debian's usual *Applications* menu, *Configuration* and *Exit*. The file is `config/fluxbox-menu`; edit the installed copy in `~/.fluxbox/menu` and choose *Reconfigure*. Delete its first line (`# panther-managed ...`) and the installer will not touch it again.

## 9.10 Installing with no internet at all

Everything the installer downloads is a Debian package (71 of them, about 160 MB: the compiler, the device-tree compiler, the browser with its WebKit engine, Geany, micro, the file manager and `alsa-utils`). Nothing else comes from the network: the kernel headers are on the board, and the Rust build has no dependencies.

1. **On a board that has internet** (a stock image is ideal), build the bundle: `scripts/make-offline-bundle.sh /path/to/output`. It writes `panther-offline-bundle-<kernel>.tar.gz`. The script computes exactly the packages a stock image lacks, using apt itself, and changes nothing on the board (it works on a *copy* of dpkg's database).
2. **Carry** that file (a USB stick, or a file share) and the repository to the offline board.
3. **There**: `tar xzf panther-offline-bundle-*.tar.gz` next to `install.sh` (a directory called `panther-offline-bundle` is found automatically), then `./install.sh`, or name it: `./install.sh --offline /media/usb/panther-offline-bundle`.

In offline mode apt is pointed at the bundle as a *local repository* (`deb [trusted=yes] file:/path ./`) with its own package-list directory, so it never reads or damages the normal lists and cannot reach the network.

*How this was tested:* on the development board the bundle's packages were removed (to look like a stock image), the default network route was deleted and `curl` confirmed the internet unreachable, then `install.sh --offline` ran to the end: apt printed only `Get: file:` lines, none for `http`, and the log contained no network references. A second test used an *older* bundle without the file manager: the install still succeeded and only warned about the missing optional packages.

**A ready-made bundle** for the PYNQ v3.1 image is attached to the project's GitHub release (`v0.2.1`):

```sh
wget https://github.com/jwkunz/pynq-panther/releases/download/v0.2.1/panther-offline-bundle-6.6.10.tar.gz
wget https://github.com/jwkunz/pynq-panther/releases/download/v0.2.1/SHA256SUMS.txt && sha256sum -c SHA256SUMS.txt
```

The bundle is valid only for the same Ubuntu release and CPU architecture (PYNQ v3.1: Ubuntu 22.04, armhf). It is too large for the git history (about 160 MB), so it is distributed as a release asset or on a USB stick, never committed. The packages are unmodified Ubuntu packages under their own licences; the manifest records versions.

## 9.11 Summary

* One command installs the whole stack; it checks first, builds against the running kernel, installs into well-defined places, and can undo itself.
* The slow step is generating the kernel's missing headers; it is cached.
* Every failure mode lands on "headless, as before", and the log says why.
