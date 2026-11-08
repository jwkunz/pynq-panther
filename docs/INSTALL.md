# Installing pynq-panther: the complete guide

This guide takes you from a PYNQ-Z2 with the standard PYNQ v3.1 SD-card image to a board that boots into a graphical desktop on its HDMI output.
> **New to the PYNQ-Z2 itself?** Read PYNQ's own [PYNQ-Z2 setup guide](https://pynq.readthedocs.io/en/v3.1/getting_started/pynq_z2_setup.html) first (power, jumpers, SD card, first boot),
> and see [PYNQ-RESOURCES.md](PYNQ-RESOURCES.md) for a map of all the official documentation. This guide starts from a board that already boots the PYNQ image.

It covers every way the board can be connected - **a normal network, a PC sharing its Wi-Fi through the Ethernet cable, or no network at all** -
and explains each step so you can fix things when something is different on your bench.

For the *why* behind the installer (kernel modules, headers, systemd), read [chapter 9 of the book](book/09-install-guide.md).
This page is the *how*.

---

## 0. Choose your path

```
                      Does the board have (or can it get) internet?
                          |
          +---------------+------------------------------+
          |                                              |
         YES                                             NO
          |                                              |
   Is it plugged into a router/switch                    |
   that provides internet (DHCP)?                        |
          |                                              |
   +------+---------+                           Can you download files on
   |                |                           another computer and carry them over?
  YES              NO                                    |
   |                |                           +-------+-------+
   |       Plug it into your PC's                |               |
   |       Ethernet port and share              YES              NO
   |       the PC's Wi-Fi  (section 3)           |         (cannot be installed:
   |                |                    Path C: offline         the installer needs
 Path A: direct     |                    bundle (section 4)      the packages once)
 (section 2)        Path B (section 3)
```

| Path | Situation | What you need | Time (typical) |
|---|---|---|---|
| **A** | Board on a network with internet (router, campus LAN) | the board, a network cable | ~10-15 min |
| **B** | Board plugged straight into a PC; the PC's Wi-Fi provides internet | the board, one cable, a Linux/Windows/macOS PC | ~10-15 min + 5 min setup |
| **C** | Board never has internet (lab, classroom, field) | another computer with internet, a USB stick or a cable | ~10 min on the board |

Time on the board is dominated by compiling the kernel modules and the Rust supervisor on two 650 MHz cores (about 2 minutes each), plus package downloads.

### You will need (all paths)

* A PYNQ-Z2 running the **PYNQ v3.1 image** (check later: `uname -r` prints `6.6.10-xilinx-v2024.1-g...`). Other images are untested.
* A good **power supply**: the 5 V barrel supply with the power jumper on *REG*. USB power is marginal and shows up as resets or lost network links under load, which is exactly what compiling is.
* About **1 GB of free space** on the SD card (`df -h /`).
* An HDMI monitor in the board's **HDMI OUT** port (the other HDMI port is an input), and a USB keyboard and mouse.
* A way to type commands on the board: SSH (section 1) is easiest.

---

## 1. Get a command line on the board

### 1.1 By SSH (recommended)

From any computer on the same network: `ssh xilinx@pynq-panther.local` (password `xilinx` on a default image).

* The name `pynq-panther.local` comes from *mDNS*; the installer sets it up (section 5.2), and PYNQ images already announce a name of this kind. If your hostname is different, or the name does not resolve, use the address instead.
* **The board always answers at `192.168.2.99`**, a fixed address it keeps in addition to whatever DHCP gives it. If your PC is on the same cable and in the `192.168.2.x` range, `ssh xilinx@192.168.2.99` works with no router at all.
* **Find the board's address** when you do not know it:
  * On Linux: `ip neigh show` after pinging, or `nmap -sn 192.168.2.0/24` (replace with your subnet).
  * On Windows: `arp -a`; on macOS: `arp -a` or `dns-sd -B _ssh._tcp`.
  * In your router's list of connected devices (look for `Pynq-Panther`).
  * With a keyboard and a monitor on the board: `ip -4 addr show eth0`.

The first connection asks you to trust the board's key; answer `yes`. If you re-flashed the SD card, SSH may warn that the host key changed: run `ssh-keygen -R pynq-panther.local` (and the same for the IP) and connect again.

### 1.2 With the USB serial console (when there is no network at all)

The micro-USB port labelled **PROG UART** carries a serial console. Connect it to your PC; it appears as `/dev/ttyUSB0` or `/dev/ttyUSB1` on Linux (a `COM` port on Windows).
Open it at **115200 baud, 8N1**: `screen /dev/ttyUSB1 115200` (leave with `Ctrl-A` then `K`), or PuTTY on Windows. Log in as `xilinx` / `xilinx`.
The serial console works even when the network does not, which makes it the safest tool for debugging a board that stopped answering.

### 1.3 With the board's own screen and keyboard

If the board already boots to a text login on HDMI, or you can plug a keyboard into it, log in there.
(Before this project is installed the board has no desktop; the image is headless.)

---

## 2. Path A - the board has internet (router or LAN)

Plug the board's Ethernet cable into a router or switch, power it, wait about two minutes for PYNQ to finish starting
(it loads its FPGA design at boot), then:

```sh
ssh xilinx@pynq-panther.local
# on the board:
ping -c 2 ports.ubuntu.com                 # must work (DNS and internet)
sudo apt-get install -y git                # only if 'git' is missing
git clone https://github.com/jwkunz/pynq-panther.git
cd pynq-panther
./install.sh                               # asks for the password once (xilinx)
```

Run long installs in a way that survives a lost connection (a sleeping laptop, a brief network drop):

```sh
sudo apt-get install -y tmux ; tmux        # then run ./install.sh inside; reattach later with: tmux attach
# or:  nohup ./install.sh --yes > ~/install.out 2>&1 &     then watch it with: tail -f ~/install.out
```

When it finishes, the monitor shows the desktop. Skip to [section 5](#5-after-the-install) for checks.

---

## 3. Path B - no router: share your PC's Wi-Fi through the Ethernet cable

Many benches have a PC on Wi-Fi and a board with only an Ethernet port. The PC can *share* its internet connection through its own Ethernet port:
the PC becomes a tiny router (it hands the board an address, forwards its traffic and translates addresses, "NAT").

```
   internet --- Wi-Fi --- [ your PC ] --- Ethernet cable --- [ PYNQ-Z2 ]
                          PC: 192.168.2.1                    board: 192.168.2.x  (and always 192.168.2.99)
```

Do the PC-side setup for your operating system, then continue at "3.4 Check that the board has internet".

### 3.1 Linux (NetworkManager - Ubuntu, Fedora, Debian desktops)

The easy way is the helper script in this repository, run on the **PC**:

```sh
git clone https://github.com/jwkunz/pynq-panther.git && cd pynq-panther    # on the PC
scripts/share-internet.sh status                  # shows which port and which internet connection it will use; changes nothing
scripts/share-internet.sh start --dry-run         # prints the exact commands
scripts/share-internet.sh start                   # does it
```

It creates a NetworkManager connection with `ipv4.method shared` on the Ethernet port (PC address `192.168.2.1/24`), which makes NetworkManager run the DHCP and DNS service and the NAT.
It refuses to continue if the Ethernet port is the connection that carries the PC's own internet, and it tells you if the port is already shared by another profile (then there is nothing to do).
When you are done: `scripts/share-internet.sh stop`.

Without the script, the same thing with `nmcli` (replace `enp0s31f6` with your Ethernet device name from `nmcli device`):

```sh
nmcli connection add type ethernet ifname enp0s31f6 con-name panther-share autoconnect no \
      ipv4.method shared ipv4.addresses 192.168.2.1/24 ipv6.method ignore
nmcli connection up panther-share
# ... install ...
nmcli connection delete panther-share
```

Or in the GUI: *Settings -> Network -> Wired -> (gear) -> IPv4 tab -> IPv4 Method: **Shared to other computers***.

**Without NetworkManager** (a server, or `systemd-networkd`): do it by hand. `eth` is the cable port, `wlan` the Wi-Fi port:

```sh
sudo ip addr add 192.168.2.1/24 dev eth
sudo sysctl -w net.ipv4.ip_forward=1
sudo iptables -t nat -A POSTROUTING -s 192.168.2.0/24 -o wlan -j MASQUERADE
sudo iptables -A FORWARD -i eth -o wlan -j ACCEPT
sudo iptables -A FORWARD -i wlan -o eth -m state --state RELATED,ESTABLISHED -j ACCEPT
```

There is no DHCP/DNS service in this variant, so on the **board** set them by hand (the board already has `192.168.2.99` on its cable, so only the route and a DNS server are missing):

```sh
sudo ip route add default via 192.168.2.1
echo "nameserver 1.1.1.1" | sudo tee /etc/resolv.conf      # temporary; the board may rewrite this file on reboot
```

Undo on the PC: `sudo iptables -t nat -D POSTROUTING -s 192.168.2.0/24 -o wlan -j MASQUERADE` (and the two `FORWARD` rules with `-D`), then `sudo sysctl -w net.ipv4.ip_forward=0`.

### 3.2 Windows 10/11 (Internet Connection Sharing)

*Not tested by the project; the steps are Microsoft's standard feature.*

1. *Settings -> Network & Internet -> Advanced network settings -> More network adapter options*.
2. Right-click your **Wi-Fi** adapter -> *Properties* -> *Sharing* tab.
3. Tick **Allow other network users to connect through this computer's Internet connection**; in the drop-down choose the **Ethernet** adapter the board is plugged into. OK.
4. Windows gives the Ethernet port the address **192.168.137.1** and acts as DHCP server; the board receives `192.168.137.x`. (The board's fixed `192.168.2.99` is a different subnet, so use the DHCP address: `arp -a` or `ping pynq-panther.local`.)

To stop: untick the same box.

### 3.3 macOS (Internet Sharing)

*Not tested by the project; the steps are Apple's standard feature.*

1. *System Settings -> General -> Sharing -> Internet Sharing* (the "i" button).
2. **Share your connection from: Wi-Fi.** **To computers using:** the Ethernet (or USB Ethernet adapter) the board is plugged into. Turn Internet Sharing on.
3. macOS uses `192.168.2.1` on the cable, the same subnet as the board's fixed address.

### 3.4 Check that the board has internet

Power the board (or re-plug its cable), wait a minute, then from the PC: `ping pynq-panther.local`, or use the address from `ip neigh show` / `arp -a`.
Log in (section 1.1) and on the **board**:

```sh
ip -4 addr show eth0           # an address from the PC's range (and 192.168.2.99)
ip route                       # must contain a line 'default via <the PC>'
ping -c 2 1.1.1.1              # internet reachable by number?
ping -c 2 ports.ubuntu.com     # ... and by name (DNS)?
```

If the first `ping` works and the second does not, DNS is the problem: see the troubleshooting table. If neither works, the sharing is not active or the board has no default route.

### 3.5 Install

Exactly as in path A:

```sh
sudo apt-get install -y git
git clone https://github.com/jwkunz/pynq-panther.git
cd pynq-panther && ./install.sh
```

Afterwards **stop sharing** on the PC (`scripts/share-internet.sh stop`, or the OS setting). The installed desktop does not need the network.

---

## 4. Path C - the board never has internet (offline install)

Everything the installer downloads is a handful of Ubuntu packages (the compiler, the browser, Geany, the file manager and their libraries: about 70 packages, about 160 MB). The kernel headers come from the board and the Rust program has no dependencies.
So an offline install needs just **two things carried to the board**: the repository and the package bundle.

### 4.1 On a computer with internet: get the files

```sh
git clone https://github.com/jwkunz/pynq-panther.git                     # the repository
cd pynq-panther

# the ready-made package bundle (Ubuntu 22.04 / armhf, for the PYNQ v3.1 image; use the newest release on the Releases page):
wget https://github.com/jwkunz/pynq-panther/releases/download/v0.2.1/panther-offline-bundle-6.6.10.tar.gz
wget https://github.com/jwkunz/pynq-panther/releases/download/v0.2.1/SHA256SUMS.txt
sha256sum -c SHA256SUMS.txt                                              # must print: ... OK
# (or with the GitHub CLI:  gh release download v0.2.1 -R jwkunz/pynq-panther)
```

Pack them into one file for transport, with the bundle **unpacked next to `install.sh`** (the installer finds a directory named `panther-offline-bundle` automatically):

```sh
tar xzf panther-offline-bundle-6.6.10.tar.gz            # creates ./panther-offline-bundle/ inside the repo folder
cd .. && tar czf pynq-panther-offline.tar.gz pynq-panther
ls -lh pynq-panther-offline.tar.gz                      # about 160 MB
```

*Making your own bundle* (a different release, or to refresh versions): on a board that has internet, `scripts/make-offline-bundle.sh /some/dir`. See chapter 9, section 9.10.

### 4.2 Move the file to the board

Pick whichever fits your situation.

**(a) A cable straight from your PC** (no router, no internet needed). Give the PC's Ethernet port an address in the board's range and copy with `scp`:

```sh
sudo ip addr add 192.168.2.1/24 dev enp0s31f6          # Linux; Windows/macOS: set a manual IP 192.168.2.1 / 255.255.255.0 on that adapter
scp pynq-panther-offline.tar.gz xilinx@192.168.2.99:~
```

**(b) A USB stick.** Copy `pynq-panther-offline.tar.gz` to the stick (FAT32 or ext4 are the safe choices). Plug it into the board's USB port, then on the board:

```sh
lsblk                                   # find it, for example /dev/sda1
sudo mount /dev/sda1 /mnt
cp /mnt/pynq-panther-offline.tar.gz ~
sudo umount /mnt                        # always unmount before pulling the stick
```

**(c) The Jupyter web page** (works over a cable even when no internet exists). Open `http://pynq-panther.local:9090` (or `http://192.168.2.99:9090`) in your PC's browser, log in (password `xilinx`), use **Upload** to send the `.tar.gz`; it lands in `~/jupyter_notebooks`.

**(d) The SD card itself.** With the card in a PC's reader, copy the file onto the small FAT partition (the one that holds `BOOT.BIN`). On the board that partition is mounted at `/boot`: `cp /boot/pynq-panther-offline.tar.gz ~`.

### 4.3 Set the board's clock

The board has **no battery-backed clock**. With internet it fetches the time itself; offline it wakes up with the last time it remembered, which can be days or years off. Building software relies on file times
(`make` decides what to rebuild by comparing them), so set the clock first (the installer warns if it looks wrong):

```sh
date                                              # does it look right?
sudo date -s "2026-11-09 14:30:00"                # set it: year-month-day hour:minute:second (your real time)
```

### 4.4 Install

```sh
cd ~ && tar xzf pynq-panther-offline.tar.gz
cd pynq-panther
./install.sh                          # finds ./panther-offline-bundle by itself and says "offline mode"
# or name it:  ./install.sh --offline /path/to/panther-offline-bundle
```

In the output you should see `offline mode: packages come from ... the network is not used`. While it runs, `apt` prints lines like `Get:1 file:/home/xilinx/pynq-panther/panther-offline-bundle ./ gcc-12 ...` - `file:`, never `http:`.
If a package the installer *must* have (the compiler, the device-tree compiler) is missing from the bundle it stops with a message naming the package; rebuild the bundle with `scripts/make-offline-bundle.sh`. The *optional* desktop applications (browser, editors, file manager, the sympy launcher fix) are installed one at a time: one missing from an older bundle only produces a warning and the rest of the install carries on.

*What offline mode guarantees:* `apt` is given the bundle as its only repository, with its own private list directory, so it can neither reach the network nor disturb the board's normal package lists. The project's own test deleted the board's internet route and installed from a bundle: every package came from `file:` URLs and the logs held no network references.

---

## 5. After the install

### 5.1 Check it worked

```sh
systemctl is-active panther-desktop panther-io panther-audio        # three lines: active
journalctl -u panther-desktop --no-pager | grep panther-session      # 'display driver ready on /dev/dri/card1', 'X is up'
ls /dev/dri                                                          # card0 (Xilinx accelerator) and card1 (ours)
DISPLAY=:0 xrandr | head -3                                          # 'HDMI-1 connected primary 1920x1080'
ls /sys/class/leds | grep -c pynqz2                                  # 10
aplay -l                                                             # 'PYNQ-Z2 audio'
```

The monitor shows a dark desktop with a terminal and a working pointer. **Right-click the desktop** for the menu: Terminal, File manager, Web browser, Jupyter notebooks, Geany, micro, display resolution, colour theme, **PYNQ help** (the official tutorial notebooks on the board, and the online PYNQ documentation).
Then reboot once (`sudo reboot`) and confirm that the desktop returns by itself after about two minutes. Jupyter keeps working at `http://pynq-panther.local:9090`.

### 5.2 The board's name on the network

The installer sets the board's **host name to `Pynq-Panther`** and makes it announce itself as **`pynq-panther.local`** using *mDNS* (multicast DNS, run by the `avahi-daemon` service), together with two service advertisements: SSH (port 22) and Jupyter (port 9090). From any computer on the same network or cable:

```sh
ssh xilinx@pynq-panther.local            # the shell
http://pynq-panther.local:9090           # Jupyter, in a browser
ping pynq-panther.local                  # is it there?
```

On the board, `hostnamectl` shows the name and `avahi-resolve -n pynq-panther.local` shows the address it announces. Linux needs `avahi`/`nss-mdns` (installed on most desktops), macOS and Windows 10/11 resolve `.local` names themselves.

* **Several boards on one network** (a classroom): they would all announce the same name, and mDNS then renames the later ones to `pynq-panther-2` and so on. Give each its own name instead: `./install.sh --hostname Pynq-Lab3` (announced as `pynq-lab3.local`).
* **Keep the board's existing name**: `./install.sh --keep-hostname`.
* **No `.local` name on a network that blocks multicast** (some campus Wi-Fi): use the board's IP address; `192.168.2.99` always works on a direct cable.

### 5.3 The pink-and-dark theme

The desktop installs with a **pink-and-dark theme**: a near-black base (`#050505`) with pinks sampled from the **Pynq Panther wallpaper** (panther pink `#ff3389`, soft pink `#ecacc4`, trace pink `#eb196f`). It covers every layer a program might take its colours from:

| Layer | What changes | Where it lives |
|---|---|---|
| Window manager (fluxbox) | title bars, borders, toolbar, right-click menu, **the wallpaper** | `~/.fluxbox/styles/PinkDark`, selected in `~/.fluxbox/init`; the picture is `/usr/local/lib/panther/theme/pynq-panther-wallpaper.png`, set through `feh` |
| Terminal (xterm) | background, text, cursor, the 16 terminal colours | `/usr/local/lib/panther/theme/Xresources` |
| GTK3 programs (Geany's window, the browser's window) | dark variant with a pink selection colour | `~/.config/gtk-3.0/settings.ini` and `gtk.css` |
| GTK2 programs (the PCManFM file manager) | the same colours | `~/.gtkrc-2.0` |
| Geany's editor | code colours: pink keywords, soft-pink functions, green strings, yellow numbers | `~/.config/geany/colorschemes/pink-dark.conf` |

**Choosing another look.** Right-click the desktop -> **Colour theme**. *Pink Dark - default* is first, followed by every other fluxbox style on the board (Emerge, BlueFlux, ubuntu-dark and the rest); the one in use is marked. Fluxbox remembers your choice. (This changes only the window manager's look; the terminal and GTK colours stay pink and dark. To drop those too, install with `--no-theme` or edit the files above.)

**The wallpaper.** The desktop shows the *Pynq Panther* picture, scaled to fit with its proportions kept (the bars, if any, are the picture's own black). Use your own image with `./install.sh --wallpaper /path/to/picture.png`, or a plain colour with `--no-wallpaper`. To change it later without re-installing: `fbsetbg -a /path/to/picture.png` sets it for this session, and putting the path in the `background.pixmap:` line of `~/.fluxbox/styles/PinkDark` makes it permanent. The wallpaper is requested by the *Pink Dark* style when the desktop starts. If you pick a different style in the *Colour theme* menu, the picture stays on screen for the rest of that session (the window colours change at once); the next time the desktop starts it uses that style's own background, and choosing *Pink Dark - default* brings the picture back at the next start.

**Your own files are safe.** Anything the installer replaces (`~/.fluxbox/init`, the GTK files, Geany's `geany.conf`) is saved once as `<file>.pre-panther`, and `./install.sh --uninstall` puts them back. `./install.sh --no-theme` skips the theme completely (including the wallpaper and the `feh` package).

**Making it yours.** The palette is a handful of `#rrggbb` values repeated in the files in `config/theme/` of this repository; change them there and re-run `./install.sh`, or edit the installed copies and choose *Reconfigure* from the desktop menu. Book chapter 13 explains how the layers fit together.

### 5.4 Day-to-day

| I want to... | Do this |
|---|---|
| change the resolution | right-click -> *Display resolution*, or `DISPLAY=:0 xrandr --output HDMI-1 --mode 1280x720` |
| run the board headless again | edit `/etc/panther/panther.conf`: `enabled = false`, reboot (or `sudo systemctl disable panther-desktop`) |
| restart the desktop | `sudo systemctl restart panther-desktop` |
| see why the desktop did not start | `journalctl -u panther-desktop --no-pager` and `/run/panther/Xorg.log` |
| update the project | `cd pynq-panther && git pull && ./install.sh` (fast: the kernel tree is cached) |
| after a kernel update | `./install.sh --rebuild` |
| change the options | `./install.sh --no-apps` (no browser/editors/file manager), `--no-audio` (no sound driver), `--no-theme` (stock colours), `--wallpaper FILE` / `--no-wallpaper`, `--hostname NAME` / `--keep-hostname` (network name), `--user NAME` (desktop account) |
| run PYNQ's own notebooks | most just work (the installer adds a guard); for HDMI, audio or other overlays: `sudo panther-mode pynq` first, `sudo panther-mode desktop` afterwards: [PYNQ-RESOURCES.md, section 6](PYNQ-RESOURCES.md) |
| remove everything | `./install.sh --uninstall` |

**Important:** reprogramming the FPGA while the desktop runs freezes the board, and a PYNQ notebook does that on its first line. The installer protects you: while the desktop runs, `BaseOverlay("base.bit")` skips the reprogramming (the FPGA already holds that design) and a *different* overlay is refused with an explanation. For notebooks that load another overlay or drive HDMI or audio, run `sudo panther-mode pynq` first (it stops the desktop, the sound card and the GPIO devices) and `sudo panther-mode desktop` when you are done. The official PYNQ tutorials are on the board and in the desktop's right-click menu, **PYNQ help**; [PYNQ-RESOURCES.md](PYNQ-RESOURCES.md) maps the whole official documentation.

---

## 6. Troubleshooting

### 6.1 Network

| Symptom | Cause and fix |
|---|---|
| `ssh: Could not resolve hostname pynq-panther.local` | mDNS is not working on your PC or network. Use the IP address (section 1.1), or `192.168.2.99` on a direct cable. |
| `No route to host` / no answer at all | cable, power or the board still starting (wait two minutes after power-on). Check the link lights. A board that stopped answering mid-install usually needs a power cycle; the serial console (1.2) shows what it was doing. |
| Board does not get an address on a shared cable | the sharing is not active; run `scripts/share-internet.sh status`. Re-plug the cable after starting sharing. The fixed `192.168.2.99` still works if the PC is in `192.168.2.x`. |
| `ping 1.1.1.1` works, `ping ports.ubuntu.com` fails | DNS. On the board: `cat /etc/resolv.conf`; add a server: `echo "nameserver 1.1.1.1" \| sudo tee /etc/resolv.conf`. With NetworkManager sharing, DNS is provided automatically. |
| `connect: Network is unreachable` | the board has no default route: `ip route` shows no `default`. `sudo ip route add default via 192.168.2.1` (the PC's address). |
| `apt` says *Release file ... is not valid yet* | the board's clock is far behind. Set it (section 4.3), or connect to the internet for a minute so it fetches the time. |
| Everything works, then the link drops during the compile | a weak power supply or a loose cable. Use the 5 V barrel supply. (On the development board the Ethernet link once dropped for about 2.5 minutes under load; the install continued unaffected, so reconnect and check `tail ~/install.out`.) |
| Windows PC: board has no internet after enabling sharing | Windows resets sharing when the Wi-Fi reconnects; untick and tick again. Use the board's DHCP address (`192.168.137.x`), not `.99`. |

### 6.2 Installer

| Symptom | Cause and fix |
|---|---|
| `expected a 32-bit ARM (armv7l) system` | you are not on the board (for example on your PC). Run the installer **on the board**. |
| `no kernel build directory for ...` | the image lacks `/lib/modules/$(uname -r)/build`. Use the PYNQ v3.1 image. |
| `no network and the build tools are not installed` | offline board: follow path C (section 4). |
| `is not an offline bundle (no Packages.gz)` | the bundle directory is not unpacked, or you pointed `--offline` at the wrong folder. It must contain `Packages.gz` and the `.deb` files. |
| a package is missing from the offline bundle | a required one stops the install; an optional one prints `WARNING: could not install the optional package ...` and the install continues. Rebuild the bundle with `scripts/make-offline-bundle.sh` on a stock PYNQ v3.1 board. |
| a menu entry (for example *sympy*) does nothing when clicked | the installer ends with a *desktop menu check* that lists menu commands pointing at missing files. The *sympy* entry was exactly this: an Ubuntu packaging bug, fixed automatically by installing the tiny `isympy3` package (book chapter 10, tale 10). |
| step 3 or 4 seems stuck | the board is slow and the compile prints nothing. Check `top` in a second session. The kernel-tree step has a 30-minute limit. If hundreds of `make` processes appear, stop and report it (book chapter 10, tale 8). |
| `Invalid module format` when the module loads | built for another kernel: `./install.sh --rebuild`. |
| Black screen after the install | X is not running: `systemctl status panther-desktop`, `journalctl -u panther-desktop`. Check the cable is in **HDMI OUT**. The board keeps working headless. |
| A PYNQ notebook stopped with "Loading ... would reprogram the FPGA while the desktop is using it" | that is the guard protecting the board: follow its message (`sudo panther-mode pynq`, run the notebook, `sudo panther-mode desktop`). |
| Board froze when the install reached its last step | an early bug, fixed in every release since (the services used to re-run PYNQ's `boot.py`). Make sure you have the current repository (`git pull`). |

---

## 7. Security notes

* The default account `xilinx` / `xilinx` is public knowledge. **Change the password** on any board that other people can reach: `passwd`.
* While you *share your PC's internet* the board can reach the internet, and anything on the cable can reach the board. Stop sharing when the install is done.
* The desktop's X server runs as root and listens on no network port (`-nolisten tcp`); the only client is the local session.
* The offline bundle contains unmodified packages from Ubuntu's public archive. `SHA256SUMS.txt` on the release lets you verify the download.

---

## 8. Cheat sheet

```sh
# --- Path A / B: board has internet ---
ssh xilinx@pynq-panther.local
git clone https://github.com/jwkunz/pynq-panther.git && cd pynq-panther && ./install.sh

# --- Path B: PC shares its Wi-Fi (Linux PC) ---
scripts/share-internet.sh start        # ... install on the board ...      scripts/share-internet.sh stop

# --- Path C: no internet on the board ---
#  PC:     git clone ... ; wget the bundle ; tar xzf bundle inside the repo ; tar czf pynq-panther-offline.tar.gz pynq-panther
#  PC:     scp pynq-panther-offline.tar.gz xilinx@192.168.2.99:~          (or USB stick)
#  board:  sudo date -s "YYYY-MM-DD HH:MM:SS" ; tar xzf pynq-panther-offline.tar.gz ; cd pynq-panther ; ./install.sh

# --- afterwards ---
ssh xilinx@pynq-panther.local          # the board announces itself by name (section 5.2)
systemctl is-active panther-desktop panther-io panther-audio
./install.sh --uninstall               # removes it all
```
