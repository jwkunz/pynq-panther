#!/usr/bin/env bash
#
# install.sh - turn a stock PYNQ-Z2 (PYNQ v3.1 image) into a board with a graphical desktop on its HDMI output.
#
#   git clone https://github.com/jwkunz/pynq-panther.git && cd pynq-panther && ./install.sh
#
# Full guide (networks, sharing a PC's Wi-Fi, offline installs, troubleshooting): docs/INSTALL.md
# What it does (each step is explained in docs/book/09-install-guide.md; it is safe to run again):
#   1. checks that this really is a PYNQ-Z2 running the expected kind of kernel
#   2. installs the build tools the kernel needs (gcc-12, device-tree-compiler)
#   3. prepares a private copy of the kernel headers (the stock copy is incomplete)            [a few minutes the first time]
#   4. builds the kernel modules and the Rust session supervisor
#   5. installs them, the configuration, a udev rule and the systemd services
#   6. enables the services at boot and starts them now
#
# Options:  --no-start    install and enable, but do not start the desktop now
#           --no-enable   install only; nothing starts at boot
#           --no-audio    do not install the experimental sound-card driver (it needs one CPU core while sound plays)
#           --no-apps     do not install the desktop applications (web browser, Geany editor, micro, PCManFM file manager)
#           --hostname N  name of the board on the network (default Pynq-Panther; announced as pynq-panther.local); --keep-hostname leaves it alone
#           --no-theme    keep the stock fluxbox/GTK/terminal colours instead of the pink-and-dark theme (and its wallpaper)
#           --wallpaper F use the picture F as the desktop wallpaper instead of the Pynq Panther one; --no-wallpaper: a plain colour
#           --offline DIR install every package from the bundle in DIR instead of the internet (see scripts/make-offline-bundle.sh);
#                         a directory named panther-offline-bundle next to this script is used automatically
#           --rebuild     rebuild the module even if the kernel did not change (needed after a kernel update)
#           --user NAME   account that runs the desktop (default: xilinx)
#           --uninstall   remove everything this script installed (packages and the kernel-header copy stay)
#           -y, --yes     do not ask questions
#           -h, --help    this text
set -euo pipefail

# ------------------------------------------------------------------------------------------------ configuration
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PREFIX=/usr/local/lib/panther            # modules, session script, dtbo, helper scripts
BIN=/usr/local/bin/panther-session
CONF_DIR=/etc/panther
KBUILD=/var/lib/panther/kbuild           # private, prepared copy of the kernel headers
LOG=/var/log/panther-install.log
KREL="$(uname -r)"
DESKTOP_USER=xilinx
START=1 ENABLE=1 REBUILD=0 UNINSTALL=0 ASSUME_YES=0 AUDIO=1 APPS=1 THEME=1 WALLPAPER=default SET_HOSTNAME=1 HOSTNAME_WANTED="Pynq-Panther" OFFLINE=""

usage() { sed -n '2,24p' "$0" | sed 's/^# \{0,1\}//'; }
while [ $# -gt 0 ]; do
  case "$1" in
    --no-start) START=0 ;;
    --no-enable) ENABLE=0; START=0 ;;
    --no-audio) AUDIO=0 ;;
    --no-apps) APPS=0 ;;
    --no-theme) THEME=0 ;;
    --wallpaper) WALLPAPER="$2"; shift ;;
    --no-wallpaper) WALLPAPER=none ;;
    --hostname) HOSTNAME_WANTED="$2"; shift ;;
    --keep-hostname) SET_HOSTNAME=0 ;;
    --offline) OFFLINE="$2"; shift ;;
    --rebuild) REBUILD=1 ;;
    --user) DESKTOP_USER="$2"; shift ;;
    --uninstall) UNINSTALL=1 ;;
    -y|--yes) ASSUME_YES=1 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "unknown option: $1"; usage; exit 2 ;;
  esac
  shift
done

[ -z "$OFFLINE" ] && [ -f "$REPO/panther-offline-bundle/Packages.gz" ] && OFFLINE="$REPO/panther-offline-bundle"
[ -n "$OFFLINE" ] && OFFLINE="$(cd "$OFFLINE" && pwd)"
if [ "$WALLPAPER" != default ] && [ "$WALLPAPER" != none ]; then
  [ -f "$WALLPAPER" ] || { echo "ERROR: --wallpaper: no such file: $WALLPAPER" >&2; exit 1; }
  WALLPAPER="$(cd "$(dirname "$WALLPAPER")" && pwd)/$(basename "$WALLPAPER")"
fi

# ------------------------------------------------------------------------------------------------ helpers
say()  { printf '\n\033[1;34m==> %s\033[0m\n' "$*"; }
note() { printf '    %s\n' "$*"; }
die()  { printf '\n\033[1;31mERROR: %s\033[0m\n' "$*" >&2; exit 1; }

# Everything that changes the system needs root. Re-run ourselves through sudo (asks for the password once).
if [ "$(id -u)" -ne 0 ]; then
  command -v sudo >/dev/null || die "this script needs root and sudo is not installed"
  exec sudo --preserve-env=HOME -- "$0" "$@"
fi
mkdir -p "$(dirname "$LOG")"
exec > >(tee -a "$LOG") 2>&1
echo "=== install.sh $(date) kernel $KREL ==="

# The account that owns the Rust toolchain (cargo) and the build output: whoever called sudo, else the desktop user.
BUILD_USER="${SUDO_USER:-$DESKTOP_USER}"
id "$BUILD_USER" >/dev/null 2>&1 || die "no such user: $BUILD_USER"
BUILD_HOME="$(getent passwd "$BUILD_USER" | cut -d: -f6)"
DESK_HOME="$(getent passwd "$DESKTOP_USER" | cut -d: -f6)"   # home of the account that runs the desktop

confirm() {
  [ "$ASSUME_YES" -eq 1 ] && return 0
  read -r -p "$1 [y/N] " a </dev/tty || return 1
  [ "$a" = y ] || [ "$a" = Y ]
}

# ------------------------------------------------------------------------------------------------ uninstall
if [ "$UNINSTALL" -eq 1 ]; then
  say "Removing pynq-panther"
  for u in panther-desktop panther-io panther-audio; do systemctl disable --now "$u" 2>/dev/null || true; done
  rmmod panther_audio 2>/dev/null || true
  rmmod panther_drm 2>/dev/null || true
  rm -f /etc/systemd/system/panther-desktop.service /etc/systemd/system/panther-io.service /etc/systemd/system/panther-audio.service
  rm -f /etc/udev/rules.d/72-panther-xrt-seat.rules "/lib/modules/$KREL/extra/panther_drm.ko" "/lib/modules/$KREL/extra/panther_audio.ko" "$BIN"
  rm -rf "$PREFIX"; rm -f /usr/local/bin/panther-browser /usr/local/bin/panther-mode
  rm -f /usr/local/share/pynq-venv/lib/python3*/site-packages/panther_pynq_guard.py /usr/local/share/pynq-venv/lib/python3*/site-packages/panther_pynq_guard.pth
  rm -f /usr/local/share/pynq-venv/lib/python3*/site-packages/__pycache__/panther_pynq_guard*
  DESK_HOME="$(getent passwd "$DESKTOP_USER" | cut -d: -f6)"
  if [ -f "$DESK_HOME/.fluxbox/menu" ] && grep -q '^# panther-managed' "$DESK_HOME/.fluxbox/menu"; then
    if [ -f "$DESK_HOME/.fluxbox/menu.pre-panther" ]; then mv -f "$DESK_HOME/.fluxbox/menu.pre-panther" "$DESK_HOME/.fluxbox/menu"; else rm -f "$DESK_HOME/.fluxbox/menu"; fi
  fi
  # theme: put the user's own files back (or remove ours)
  FB="$DESK_HOME/.fluxbox"; GT="$DESK_HOME/.config/gtk-3.0"; GY="$DESK_HOME/.config/geany"
  if [ -f "$FB/init.pre-panther" ]; then mv -f "$FB/init.pre-panther" "$FB/init"
  elif [ -f "$FB/init" ]; then sed -i 's|^session.styleFile:.*|session.styleFile:\t/usr/share/fluxbox/styles/ubuntu-dark|' "$FB/init"; fi
  rm -f "$FB/styles/PinkDark" "$GY/colorschemes/pink-dark.conf"
  for f in "$GT/settings.ini" "$GT/gtk.css"; do
    if [ -f "$f.pre-panther" ]; then mv -f "$f.pre-panther" "$f"; elif grep -q 'panther-managed' "$f" 2>/dev/null; then rm -f "$f"; fi
  done
  [ -f "$GY/geany.conf.pre-panther" ] && mv -f "$GY/geany.conf.pre-panther" "$GY/geany.conf"
  G2="$DESK_HOME/.gtkrc-2.0"
  if [ -f "$G2.pre-panther" ]; then mv -f "$G2.pre-panther" "$G2"; elif grep -q 'panther-managed' "$G2" 2>/dev/null; then rm -f "$G2"; fi
  rm -f /etc/avahi/services/panther-ssh.service /etc/avahi/services/panther-jupyter.service
  systemctl reload avahi-daemon 2>/dev/null || true
  depmod -a; systemctl daemon-reload; udevadm control --reload || true
  note "kept: $CONF_DIR (your settings), $KBUILD (kernel header copy), installed packages, the host name, $REPO"
  say "Done. The board is back to its stock (headless) behaviour; reboot to stop X completely if it is still running."
  exit 0
fi

# ------------------------------------------------------------------------------------------------ 1. checks
say "1/6  Checking the board"
[ "$(uname -m)" = armv7l ] || die "expected a 32-bit ARM (armv7l) system, found $(uname -m); this is for the PYNQ-Z2"
[ -d /sys/class/fpga_manager/fpga0 ] || die "no FPGA manager found: is this a Zynq board?"
command -v systemctl >/dev/null || die "systemd is required"
[ -d "/lib/modules/$KREL/build" ] || die "no kernel build directory for $KREL (/lib/modules/$KREL/build)"
grep -q "CONFIG_DRM=y" "/lib/modules/$KREL/build/.config" || die "this kernel has no DRM support"
grep -q "CONFIG_MODULES=y" "/lib/modules/$KREL/build/.config" || die "this kernel cannot load modules"
note "kernel $KREL, $(grep -c ^processor /proc/cpuinfo) CPUs, $(free -m | awk '/Mem:/{print $2}') MB RAM"
# The board has no battery-backed clock. Offline it can wake up days or years behind, which confuses make (it compares file times)
# and apt (it refuses package lists "not valid yet"). Compare with the newest thing we know the time of: this repository's last commit.
NEWEST="$(git -C "$REPO" log -1 --format=%ct 2>/dev/null || true)"; [ -n "$NEWEST" ] || NEWEST="$(stat -c %Y "$REPO/install.sh")"
if [ "$(date +%s)" -lt "$NEWEST" ]; then
  note "WARNING: the system clock says $(date '+%F %T'), which is EARLIER than this repository's newest file ($(date -d "@$NEWEST" '+%F %T'))."
  note "         Set it first, then run this script again:   sudo date -s \"YYYY-MM-DD HH:MM:SS\"   (docs/INSTALL.md, section 4.3)"
  [ "$ASSUME_YES" -eq 1 ] || confirm "Continue anyway?" || die "stopped: set the clock first"
fi
[ "$(cat /sys/class/fpga_manager/fpga0/state)" = operating ] || note "warning: the FPGA is not configured yet; the desktop will wait for the PYNQ base overlay at boot"
if [ -n "$OFFLINE" ]; then
  [ -f "$OFFLINE/Packages.gz" ] || die "$OFFLINE is not an offline bundle (no Packages.gz); build one with scripts/make-offline-bundle.sh"
  note "offline mode: packages come from $OFFLINE; the network is not used"
elif ! ping -c1 -W3 ports.ubuntu.com >/dev/null 2>&1; then
  dpkg -s gcc-12 device-tree-compiler >/dev/null 2>&1 || die "no network and the build tools are not installed (need: gcc-12 device-tree-compiler). Use --offline with a bundle."
  note "warning: no network, using already-installed packages"
fi
if [ -x "$BUILD_HOME/.cargo/bin/cargo" ]; then CARGO="$BUILD_HOME/.cargo/bin/cargo"; elif command -v cargo >/dev/null; then CARGO="$(command -v cargo)"; else
  die "Rust (cargo) not found for user $BUILD_USER. The PYNQ image normally has it in ~/.cargo; otherwise install it with: curl https://sh.rustup.rs -sSf | sh"
fi
note "Rust: $(sudo -u "$BUILD_USER" -H "$CARGO" --version 2>&1 | head -1)"

# ------------------------------------------------------------------------------------------------ 2. packages
say "2/6  Installing packages"
# apt_install PKG...  installs only what is missing, from the internet or - in offline mode - from the bundle (as a local repository).
apt_install() {
  local missing="" p
  for p in "$@"; do dpkg -s "$p" >/dev/null 2>&1 || missing="$missing $p"; done
  [ -z "$missing" ] && return 0
  note "installing:$missing"
  if [ -n "$OFFLINE" ]; then
    local st=/var/lib/panther/apt-offline; mkdir -p "$st/lists/partial"
    echo "deb [trusted=yes] file:$OFFLINE ./" > "$st/sources.list"
    local o=(-o "Dir::Etc::sourcelist=$st/sources.list" -o "Dir::Etc::sourceparts=/dev/null" -o "Dir::State::lists=$st/lists" -o "Acquire::http::Proxy=http://127.0.0.1:9" -o "Acquire::https::Proxy=http://127.0.0.1:9")
    apt-get "${o[@]}" update -qq
    DEBIAN_FRONTEND=noninteractive apt-get "${o[@]}" install -y --no-install-recommends $missing
  else
    apt-get update -qq || true
    DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends $missing
  fi
}
# apt_install_optional PKG...  one package at a time; a package that cannot be installed (for example one that an older offline bundle
# does not contain) only produces a warning, so a nice-to-have never stops the install of the driver.
apt_install_optional() {
  local p
  for p in "$@"; do
    apt_install "$p" || note "WARNING: could not install the optional package '$p'; continuing without it"
  done
}
note "the kernel was compiled with GCC 12; the image's gcc is 11, which rejects some of the kernel's compiler flags"
apt_install gcc-12 device-tree-compiler
# X and its tools ship with the PYNQ image; make sure of it instead of assuming
apt_install xserver-xorg-core xserver-xorg-input-libinput xserver-xorg-video-fbdev fluxbox xterm x11-xserver-utils x11-apps
if [ "$APPS" -eq 1 ]; then
  note "desktop applications: web browser (Epiphany), Geany editor, micro, PCManFM file manager"
  apt_install_optional epiphany-browser geany micro pcmanfm
  # Ubuntu 22.04 packaging bug: python3-sympy (the PYNQ image has it) adds a menu entry that runs /usr/bin/isympy, but that launcher
  # is only installed by the separate package isympy3, so the "sympy" menu entry silently did nothing. isympy3 is a few kilobytes.
  if dpkg -s python3-sympy >/dev/null 2>&1; then apt_install_optional isympy3; fi
fi
[ "$AUDIO" -eq 1 ] && apt_install_optional alsa-utils
# feh draws the wallpaper picture (fluxbox calls it through its helper fbsetbg)
if [ "$THEME" -eq 1 ] && [ "$WALLPAPER" != none ]; then apt_install_optional feh; fi

# ------------------------------------------------------------------------------------------------ 3. kernel headers
say "3/6  Preparing the kernel build tree"
STAMP="$KBUILD/.panther-prepared"
if [ -f "$STAMP" ] && [ "$(cat "$STAMP")" = "$KREL" ] && [ "$REBUILD" -eq 0 ]; then
  note "already prepared for $KREL ($KBUILD)"
else
  note "copying the kernel headers and generating the missing files (a few minutes on this board)"
  rm -rf "$KBUILD"; mkdir -p "$KBUILD"
  cp -a "/lib/modules/$KREL/build/." "$KBUILD/"
  # The image's header tree lacks the kernel's root 'Kbuild' file. Without it the last step of 'make modules_prepare'
  # ("$(MAKE) $(build)=. prepare") falls back to reading the top-level Makefile again and recurses without end. That step only
  # regenerates headers (bounds.h, asm-offsets.h, ...) that the image already ships, so in OUR PRIVATE COPY we delete that one
  # line; everything else modules_prepare does (architecture headers, host tools such as modpost, module.lds) still runs.
  if [ ! -f "$KBUILD/Kbuild" ]; then
    [ "$(grep -c '^	\$(Q)\$(MAKE) \$(build)=\. prepare$' "$KBUILD/Makefile")" -eq 1 ] || die "unexpected kernel Makefile: cannot apply the one-line workaround"
    sed -i '/^	\$(Q)\$(MAKE) \$(build)=\. prepare$/d' "$KBUILD/Makefile"
  fi
  timeout 1800 make -C "$KBUILD" CC=gcc-12 ARCH=arm modules_prepare >/dev/null 2>"$LOG.kbuild-warnings" || die "preparing the kernel tree failed; see $LOG.kbuild-warnings"
  [ -f "$KBUILD/arch/arm/include/generated/asm/rwonce.h" ] && [ -x "$KBUILD/scripts/mod/modpost" ] || die "the kernel tree is still incomplete after preparation"
  echo "$KREL" > "$STAMP"
fi

# ------------------------------------------------------------------------------------------------ 4. build
say "4/6  Building"
MODULES="panther_drm"
[ "$AUDIO" -eq 1 ] && MODULES="$MODULES panther_audio"
for m in $MODULES; do
  note "kernel module $m"
  make -C "$REPO/kmod/$m" KDIR="$KBUILD" clean >/dev/null 2>&1 || true
  make -C "$REPO/kmod/$m" KDIR="$KBUILD" >/dev/null
  modinfo "$REPO/kmod/$m/$m.ko" | grep -q "vermagic:.*${KREL%%-g*}" || die "$m.ko was not built for kernel $KREL"
done
note "session supervisor (Rust)"
chown -R "$BUILD_USER" "$REPO/session"
sudo -u "$BUILD_USER" -H bash -c "cd '$REPO/session' && '$CARGO' build --release --offline" 2>&1 | tail -2
dtc -@ -I dts -O dtb -W no-unit_address_vs_reg -o "$REPO/config/panther-io.dtbo" "$REPO/config/panther-io.dts" 2>/dev/null
note "devicetree overlay panther-io.dtbo"

# ------------------------------------------------------------------------------------------------ 5. install
say "5/6  Installing"
install -d "$PREFIX"
for m in $MODULES; do
  install -D -m 644 "$REPO/kmod/$m/$m.ko" "/lib/modules/$KREL/extra/$m.ko"
  install -m 644 "$REPO/kmod/$m/$m.ko" "$PREFIX/$m.ko"
done
depmod -a
install -m 755 "$REPO/session/target/release/panther-session" "$BIN"
install -m 755 "$REPO/config/session.sh" "$PREFIX/session.sh"
install -m 755 "$REPO/scripts/panther-io-load.sh" "$PREFIX/panther-io-load.sh"
install -m 644 "$REPO/config/panther-io.dtbo" "$PREFIX/panther-io.dtbo"
install -m 755 "$REPO/config/panther-browser" /usr/local/bin/panther-browser
install -m 755 "$REPO/scripts/panther-mode" /usr/local/bin/panther-mode
install -m 755 "$REPO/scripts/record-base-overlay" "$PREFIX/record-base-overlay"
# the PYNQ guard: lets stock PYNQ notebooks load the base overlay without reprogramming the FPGA under the desktop (docs/PYNQ-RESOURCES.md)
PYSP="$(ls -d /usr/local/share/pynq-venv/lib/python3*/site-packages 2>/dev/null | head -1)"
if [ -n "$PYSP" ]; then
  install -m 644 "$REPO/config/pynq-guard/panther_pynq_guard.py" "$REPO/config/pynq-guard/panther_pynq_guard.pth" "$PYSP/"
  note "PYNQ guard installed: stock notebooks may load base.bit while the desktop runs"
else
  note "WARNING: no PYNQ virtual environment found, so PYNQ notebooks are NOT protected: use 'sudo panther-mode pynq' before them"
fi
if [ "$APPS" -eq 1 ]; then
  # the fluxbox application menu of the desktop user: our entries on top, Debian's generated menu included below
  DESK_HOME="$(getent passwd "$DESKTOP_USER" | cut -d: -f6)"
  MENU="$DESK_HOME/.fluxbox/menu"
  install -d -o "$DESKTOP_USER" "$DESK_HOME/.fluxbox"
  if [ -f "$MENU" ] && ! grep -q '^# panther-managed' "$MENU" && [ ! -f "$MENU.pre-panther" ]; then
    cp -p "$MENU" "$MENU.pre-panther"; note "your previous fluxbox menu is saved as $MENU.pre-panther"
  fi
  if [ ! -f "$MENU" ] || grep -q '^# panther-managed' "$MENU" || [ -f "$MENU.pre-panther" ]; then
    install -m 644 -o "$DESKTOP_USER" "$REPO/config/fluxbox-menu" "$MENU"
    note "fluxbox menu installed: right-click the desktop"
  fi
fi
if [ -e "$CONF_DIR/panther.conf" ]; then
  note "keeping your existing $CONF_DIR/panther.conf (new defaults in $PREFIX/panther.conf.default)"
  install -m 644 "$REPO/config/panther.conf" "$PREFIX/panther.conf.default"
else
  install -D -m 644 "$REPO/config/panther.conf" "$CONF_DIR/panther.conf"
fi
sed -i "s/^user *=.*/user = $DESKTOP_USER/" "$CONF_DIR/panther.conf"
install -m 644 "$REPO/config/panther-desktop.service" /etc/systemd/system/panther-desktop.service
install -m 644 "$REPO/config/panther-io.service" /etc/systemd/system/panther-io.service
[ "$AUDIO" -eq 1 ] && install -m 644 "$REPO/config/panther-audio.service" /etc/systemd/system/panther-audio.service
install -m 644 "$REPO/config/72-panther-xrt-seat.rules" /etc/udev/rules.d/72-panther-xrt-seat.rules
udevadm control --reload
udevadm trigger --subsystem-match=drm --action=change || true
systemctl daemon-reload

# ---- the pink-and-dark theme: fluxbox style, terminal colours, GTK3 colours, Geany colour scheme.
# Anything of yours that it replaces is saved once as <file>.pre-panther, and --uninstall puts it back.
if [ "$THEME" -eq 1 ]; then
  note "theme: pink and dark"
  install -d "$PREFIX/theme"; install -m 644 "$REPO/config/theme/Xresources" "$PREFIX/theme/Xresources"
  FB="$DESK_HOME/.fluxbox"; INIT="$FB/init"
  install -d -o "$DESKTOP_USER" "$FB" "$FB/styles"
  install -m 644 -o "$DESKTOP_USER" "$REPO/config/theme/PinkDark" "$FB/styles/PinkDark"
  # the wallpaper: the Pynq Panther picture (default), a picture of the user's, or none (the style then keeps a flat colour)
  install -m 644 "$REPO/config/theme/pynq-panther-wallpaper.png" "$PREFIX/theme/pynq-panther-wallpaper.png"
  case "$WALLPAPER" in
    default) note "wallpaper: Pynq Panther" ;;
    none) sed -i -e 's|^background:.*|background:\t\t\t\tflat|' -e '/^background.pixmap:/d' "$FB/styles/PinkDark"; note "wallpaper: none (flat colour)" ;;
    *) CUSTOM="$PREFIX/theme/wallpaper-custom.${WALLPAPER##*.}"; install -m 644 "$WALLPAPER" "$CUSTOM"
       sed -i "s|^background.pixmap:.*|background.pixmap:\t\t\t$CUSTOM|" "$FB/styles/PinkDark"; note "wallpaper: $WALLPAPER" ;;
  esac
  [ -f "$INIT" ] || { cp /etc/X11/fluxbox/init "$INIT" 2>/dev/null || : > "$INIT"; chown "$DESKTOP_USER" "$INIT"; }
  [ -f "$INIT.pre-panther" ] || cp -p "$INIT" "$INIT.pre-panther"
  if grep -q '^session.styleFile' "$INIT"; then sed -i 's|^session.styleFile:.*|session.styleFile:\t~/.fluxbox/styles/PinkDark|' "$INIT"
  else printf 'session.styleFile:\t~/.fluxbox/styles/PinkDark\n' >> "$INIT"; fi
  GT="$DESK_HOME/.config/gtk-3.0"
  install -d -o "$DESKTOP_USER" "$DESK_HOME/.config" "$GT"
  for pair in gtk-settings.ini:settings.ini gtk.css:gtk.css; do
    f="$GT/${pair##*:}"
    if [ -f "$f" ] && ! grep -q 'panther-managed' "$f" && [ ! -f "$f.pre-panther" ]; then cp -p "$f" "$f.pre-panther"; fi
    install -m 644 -o "$DESKTOP_USER" "$REPO/config/theme/${pair%%:*}" "$f"
  done
  # GTK2 programs (Ubuntu's PCManFM file manager is one) read ~/.gtkrc-2.0, not the GTK3 files above
  G2="$DESK_HOME/.gtkrc-2.0"
  if [ -f "$G2" ] && ! grep -q 'panther-managed' "$G2" && [ ! -f "$G2.pre-panther" ]; then cp -p "$G2" "$G2.pre-panther"; fi
  install -m 644 -o "$DESKTOP_USER" "$REPO/config/theme/gtkrc-2.0" "$G2"
  if [ "$APPS" -eq 1 ]; then
    GY="$DESK_HOME/.config/geany"; GC="$GY/geany.conf"
    install -d -o "$DESKTOP_USER" "$GY" "$GY/colorschemes"
    install -m 644 -o "$DESKTOP_USER" "$REPO/config/theme/geany-pink-dark.conf" "$GY/colorschemes/pink-dark.conf"
    if [ -f "$GC" ]; then
      [ -f "$GC.pre-panther" ] || cp -p "$GC" "$GC.pre-panther"
      if grep -q '^color_scheme=' "$GC"; then sed -i 's|^color_scheme=.*|color_scheme=pink-dark.conf|' "$GC"; else sed -i '/^\[geany\]/a color_scheme=pink-dark.conf' "$GC"; fi
    else printf '[geany]\ncolor_scheme=pink-dark.conf\n' > "$GC"; chown "$DESKTOP_USER" "$GC"; fi
  fi
fi

# ---- the board's name on the network: host name, announced as <name>.local through mDNS (avahi)
if [ "$SET_HOSTNAME" -eq 1 ]; then
  MDNS_LABEL="$(printf '%s' "$HOSTNAME_WANTED" | tr 'A-Z' 'a-z' | sed 's/[^a-z0-9-]/-/g')"
  note "network name: $HOSTNAME_WANTED, announced as $MDNS_LABEL.local"
  # /etc/hosts first, so that sudo never complains about an unknown host name
  if grep -q '^127\.0\.1\.1' /etc/hosts; then sed -i "s/^127\.0\.1\.1.*/127.0.1.1    $HOSTNAME_WANTED/" /etc/hosts
  else echo "127.0.1.1    $HOSTNAME_WANTED" >> /etc/hosts; fi
  hostnamectl set-hostname "$HOSTNAME_WANTED"
  apt_install_optional avahi-daemon libnss-mdns avahi-utils
  AV=/etc/avahi/avahi-daemon.conf
  if [ -f "$AV" ]; then
    [ -f "$AV.pre-panther" ] || cp -p "$AV" "$AV.pre-panther"
    avahi_set() {   # avahi_set KEY VALUE: replace the key (even if commented out) or add it under [server]
      if grep -qE "^[#; ]*$1=" "$AV"; then sed -i -E "0,/^[#; ]*$1=.*/s||$1=$2|" "$AV"; else sed -i "/^\[server\]/a $1=$2" "$AV"; fi
    }
    avahi_set host-name "$MDNS_LABEL"; avahi_set domain-name local
    install -d /etc/avahi/services
    cat > /etc/avahi/services/panther-ssh.service <<AVS
<?xml version="1.0" standalone='no'?>
<!DOCTYPE service-group SYSTEM "avahi-service.dtd">
<service-group><name replace-wildcards="yes">%h SSH</name><service><type>_ssh._tcp</type><port>22</port></service></service-group>
AVS
    cat > /etc/avahi/services/panther-jupyter.service <<AVS
<?xml version="1.0" standalone='no'?>
<!DOCTYPE service-group SYSTEM "avahi-service.dtd">
<service-group><name replace-wildcards="yes">%h Jupyter</name><service><type>_http._tcp</type><port>9090</port></service></service-group>
AVS
    systemctl enable avahi-daemon >/dev/null 2>&1 || true
    systemctl restart avahi-daemon || note "WARNING: avahi-daemon did not restart; the name $MDNS_LABEL.local may not be announced"
  else
    note "WARNING: avahi is not installed, so $MDNS_LABEL.local will not be announced (use the IP address)"
  fi
fi

# Sanity check of the desktop menu: report any entry whose command names a file that does not exist (the sympy bug above was one).
if [ "$APPS" -eq 1 ]; then
  BROKEN=""
  for f in /etc/X11/fluxbox/fluxbox-menu "$REPO/config/fluxbox-menu"; do
    for path in $(grep -oE '\{[^}]*\}' "$f" 2>/dev/null | tr -d '{}' | grep -oE '/usr/[A-Za-z0-9_./+-]+' | sort -u); do
      [ -e "$path" ] || BROKEN="$BROKEN $path"
    done
  done
  if [ -n "$BROKEN" ]; then note "WARNING: these desktop menu entries point at files that do not exist:$BROKEN"
  else note "desktop menu check: every command it names exists"; fi
fi

# ------------------------------------------------------------------------------------------------ 6. enable and start
say "6/6  Enabling the services"
if [ "$ENABLE" -eq 1 ]; then
  systemctl enable panther-io.service panther-desktop.service
  [ "$AUDIO" -eq 1 ] && systemctl enable panther-audio.service
  note "enabled at boot: panther-io (buttons, switches, LEDs), panther-desktop (HDMI desktop)$([ "$AUDIO" -eq 1 ] && echo ", panther-audio (sound)")"
else
  note "not enabled at boot (--no-enable). Enable later with: sudo systemctl enable panther-io panther-desktop"
fi
if [ "$START" -eq 1 ]; then
  # Order matters: take the desktop down first (it uses the input devices and the display driver we are about to replace),
  # and make sure nothing re-runs PYNQ's boot.py, which would reload the FPGA under a running display (see chapter 10, Tale 9).
  systemctl stop panther-desktop.service 2>/dev/null || true
  rmmod panther_drm 2>/dev/null || true
  systemctl restart panther-io.service || note "panther-io did not start (see: journalctl -u panther-io)"
  if [ "$AUDIO" -eq 1 ]; then
    rmmod panther_audio 2>/dev/null || true
    systemctl restart panther-audio.service || note "panther-audio did not start (see: dmesg | grep panther_audio)"
  fi
  systemctl start panther-desktop.service
  note "waiting for the desktop..."
  for _ in $(seq 1 60); do pgrep -x Xorg >/dev/null && break; sleep 1; done
  if pgrep -x Xorg >/dev/null; then
    note "Xorg is running; the desktop should be on the HDMI monitor"
  else
    note "X did not start. The board stays usable as before. Look at: journalctl -u panther-desktop ; /run/panther/Xorg.log"
  fi
fi
say "Installed. Log: $LOG"
[ "$SET_HOSTNAME" -eq 1 ] && echo "  Reach the board at:    ssh $DESKTOP_USER@$MDNS_LABEL.local     http://$MDNS_LABEL.local:9090  (Jupyter)"
echo "  New to the PYNQ-Z2?    docs/PYNQ-RESOURCES.md (official PYNQ documentation); right-click the desktop -> PYNQ help"
echo "  PYNQ notebooks:        the ones that load base.bit just work (guard); for other overlays, HDMI or audio: sudo panther-mode pynq ... sudo panther-mode desktop"
echo "  Turn the desktop off:  set enabled = false in $CONF_DIR/panther.conf   (or: sudo systemctl disable panther-desktop)"
echo "  Remove everything:     ./install.sh --uninstall"
