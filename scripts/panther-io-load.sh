#!/bin/sh
# panther-io-load.sh - apply the GPIO devicetree overlay (buttons, switches, LEDs) once the base bitstream is loaded.
#   usage: panther-io-load.sh [load|unload]       installed as /usr/local/lib/panther/panther-io-load.sh
set -e
DTBO=/usr/local/lib/panther/panther-io.dtbo
CFG=/sys/kernel/config/device-tree/overlays/panther-io
case "${1:-load}" in
load)
    [ "$(cat /sys/class/fpga_manager/fpga0/state 2>/dev/null)" = "operating" ] || { echo "PL not configured; not loading"; exit 0; }
    mountpoint -q /sys/kernel/config || mount -t configfs configfs /sys/kernel/config
    [ -d "$CFG" ] && rmdir "$CFG"
    mkdir "$CFG"
    cat "$DTBO" > "$CFG/dtbo"
    echo "panther-io overlay: $(cat "$CFG/status")"
    ;;
unload)
    [ -d "$CFG" ] && rmdir "$CFG"
    ;;
esac
