#!/bin/bash
# pynq_guard_selftest.sh - check the PYNQ guard on a board WITHOUT any risk to the display: the desktop is stopped first, and the guard is told to
# behave as if it were running (PANTHER_PYNQ_GUARD_FORCE_ACTIVE=1). pynq_guard_trace.py counts every PS-clock write, device shutdown and
# fpga_manager write PYNQ attempts. A "skip" must show zeros; the stock path must show real writes.
#   sudo tests/pynq_guard_selftest.sh        (takes about a minute; ends by bringing the desktop back)
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
[ "$(id -u)" -eq 0 ] || { echo "run as root: sudo $0"; exit 1; }
SP=$(ls -d /usr/local/share/pynq-venv/lib/python3*/site-packages | head -1)
BASE="$SP/pynq/overlays/base/base.bit"
run() { ( set +u; . /etc/environment; for f in /etc/profile.d/*.sh; do . "$f"; done; cd /tmp; env "$@" 2>&1 | grep -v '^$' | tail -8 ); }
echo "== stopping the desktop (nothing below can reach the display)"; panther-mode pynq | tail -3
mkdir -p /run/panther; sha1sum "$BASE" | cut -d' ' -f1 > /run/panther/pl-loaded
echo; echo "== A. the FPGA holds the requested design, desktop 'running' -> must SKIP: all counters 0"
run PANTHER_PYNQ_GUARD_FORCE_ACTIVE=1 python3 "$HERE/pynq_guard_trace.py" same
echo; echo "== B. FPGA 'holds a different design' -> must REFUSE, counters 0"
echo deadbeefdeadbeefdeadbeefdeadbeefdeadbeef > /run/panther/pl-loaded
run PANTHER_PYNQ_GUARD_FORCE_ACTIVE=1 python3 -c "
from pynq.overlays.base import BaseOverlay
try: BaseOverlay('base.bit'); print('   NO ERROR (bad)')
except RuntimeError as e: print('   refused:', str(e).splitlines()[0][:80])"
echo; echo "== C. stock behaviour (desktop not running) -> programs for real: counters > 0"
run python3 "$HERE/pynq_guard_trace.py" stock
echo; echo "== bringing the desktop back"; panther-mode desktop | tail -4
