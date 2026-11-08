#!/usr/bin/env bash
#
# share-internet.sh - on a LINUX PC with NetworkManager: share the PC's Wi-Fi (or any connection that has internet) with a PYNQ-Z2
# plugged into the PC's Ethernet port by a cable. The board then has internet for ./install.sh with no router involved.
#
#   scripts/share-internet.sh status                  show what would be used and what is shared now (changes nothing)
#   scripts/share-internet.sh start [--dry-run]       create and start the sharing connection
#   scripts/share-internet.sh stop                    stop sharing and remove the connection
#
#   Options (before or after the command):  --wired IFACE   Ethernet port the board is plugged into (default: first Ethernet device)
#                                           --subnet CIDR    PC's address on the cable (default 192.168.2.1/24: the board also answers at 192.168.2.99)
#                                           --dry-run        print the commands instead of running them
#
# How it works: NetworkManager's "shared" IPv4 method gives the cable its own small network, answers the board's DHCP request,
# runs a DNS forwarder and switches on NAT (masquerading) out through whichever connection has the default route (your Wi-Fi).
# `stop` undoes all of it. Nothing is made permanent: the connection is created with autoconnect off.
#
# Windows and macOS have the same feature built in (Internet Connection Sharing / Internet Sharing): see docs/INSTALL.md.
set -euo pipefail

CON=panther-share
SUBNET=192.168.2.1/24
WIRED=""
DRY=0
CMD="${1:-status}"; [ $# -gt 0 ] && shift || true
while [ $# -gt 0 ]; do
  case "$1" in
    --wired) WIRED="$2"; shift ;;
    --subnet) SUBNET="$2"; shift ;;
    --dry-run) DRY=1 ;;
    -h|--help) sed -n '2,19p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
  shift
done

die() { echo "ERROR: $*" >&2; exit 1; }
run() { if [ "$DRY" -eq 1 ]; then echo "  would run: $*"; else echo "  + $*"; "$@"; fi; }

command -v nmcli >/dev/null || die "NetworkManager's nmcli is not installed. See docs/INSTALL.md for the manual method (iptables)."
systemctl is-active --quiet NetworkManager || die "NetworkManager is not running."

# which devices are what
first_dev() { nmcli -t -f DEVICE,TYPE device | awk -F: -v t="$1" '$2==t {print $1; exit}'; }
[ -n "$WIRED" ] || WIRED="$(first_dev ethernet)"
[ -n "$WIRED" ] || die "no Ethernet device found; use --wired IFACE"
DEFROUTE_DEV="$(ip route show default 2>/dev/null | awk '{for(i=1;i<NF;i++) if($i=="dev"){print $(i+1); exit}}')"
SHARE_IP="${SUBNET%/*}"

# is this Ethernet port already shared by some other NetworkManager profile?
existing_share() {
  local c; c="$(nmcli -g GENERAL.CONNECTION device show "$WIRED" 2>/dev/null | head -1)"
  [ -n "$c" ] && [ "$c" != "--" ] && [ "$(nmcli -g ipv4.method connection show "$c" 2>/dev/null)" = "shared" ] && echo "$c"
}

status() {
  local ex; ex="$(existing_share || true)"
  [ -n "$ex" ] && echo "NOTE: $WIRED is ALREADY shared by the NetworkManager profile '$ex' (ipv4.method shared): nothing more to do."
  echo "Ethernet port for the board : $WIRED"
  echo "Internet comes in through   : ${DEFROUTE_DEV:-<no default route: this PC has no internet>}"
  if nmcli -t -f NAME connection show | grep -qx "$CON"; then
    echo "Sharing connection '$CON'   : exists ($(nmcli -t -f GENERAL.STATE connection show "$CON" 2>/dev/null | cut -d: -f2 | head -1 || true))"
  else
    echo "Sharing connection '$CON'   : not created"
  fi
  echo "Neighbours seen on $WIRED   :"; ip -4 neigh show dev "$WIRED" 2>/dev/null | sed 's/^/    /' || true
}

case "$CMD" in
  status) status ;;
  start)
    [ -n "$DEFROUTE_DEV" ] || die "this PC has no internet connection to share."
    if ex="$(existing_share)" && [ -n "$ex" ] && [ "$ex" != "$CON" ]; then
      echo "$WIRED is already shared by the profile '$ex'; leaving it as it is."
      echo "A board on that cable already has internet through '$DEFROUTE_DEV'. (To undo it yourself: nmcli connection down '$ex')"
      exit 0
    fi
    if [ "$DEFROUTE_DEV" = "$WIRED" ]; then
      die "$WIRED is the connection that carries this PC's internet, so it cannot also be the cable to the board.
       Connect this PC to the internet by Wi-Fi (disconnect the cable's network), or pick the port the board is on with --wired."
    fi
    echo "Sharing '$DEFROUTE_DEV' (internet) with a board on '$WIRED', PC address $SUBNET"
    if nmcli -t -f NAME connection show | grep -qx "$CON"; then run nmcli connection delete "$CON"; fi
    run nmcli connection add type ethernet ifname "$WIRED" con-name "$CON" autoconnect no \
        ipv4.method shared ipv4.addresses "$SUBNET" ipv6.method ignore
    run nmcli connection up "$CON"
    cat <<EOF

Done. Plug the board's Ethernet cable into $WIRED (or leave it plugged in and reboot the board / re-plug the cable).
In about 30 seconds the board gets an address like ${SHARE_IP%.*}.x from this PC, and also always answers at 192.168.2.99.
Find it:   ip neigh show dev $WIRED      ping pynq-panther.local      ssh xilinx@pynq-panther.local   (password: xilinx)
When the install is finished:   $0 stop
EOF
    ;;
  stop)
    if nmcli -t -f NAME connection show | grep -qx "$CON"; then
      run nmcli connection down "$CON" || true
      run nmcli connection delete "$CON"
      echo "Sharing stopped and the connection removed. '$WIRED' returns to its normal profile when you reconnect it."
    else
      echo "Nothing to stop: connection '$CON' does not exist."
    fi
    ;;
  *) die "unknown command '$CMD' (status | start | stop)" ;;
esac
