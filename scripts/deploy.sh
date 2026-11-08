#!/bin/bash
# deploy.sh - from the PC: copy the sources to the board (default pynq-panther.local) and build them there.
#   usage: scripts/deploy.sh [user@host]      (asks for the board password unless you use keys)
set -euo pipefail
BOARD="${1:-xilinx@pynq-panther.local}"
cd "$(dirname "$0")/.."
tar cf - --exclude=target --exclude="*.ko" --exclude=.git . | ssh "$BOARD" "rm -rf ~/pynq-panther && mkdir ~/pynq-panther && tar xf - -C ~/pynq-panther"
ssh -t "$BOARD" 'cd ~/pynq-panther && ./install.sh'
