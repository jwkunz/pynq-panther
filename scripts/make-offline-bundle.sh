#!/usr/bin/env bash
#
# make-offline-bundle.sh - collect every .deb the installer needs, so a board with no internet can be set up from a USB stick.
#
#   Run ON A BOARD WITH INTERNET (a stock PYNQ-Z2 image, or any board - see below):
#       scripts/make-offline-bundle.sh [output-dir]
#   Produces <output-dir>/panther-offline-bundle-<kernel>.tar.gz   (default output-dir: ./offline-out)
#
#   On the offline board:  tar xzf panther-offline-bundle-*.tar.gz   ->   ./install.sh --offline panther-offline-bundle
#
# Exactly which packages go in? The ones a STOCK image does not already have: the packages in $TARGETS and every dependency that
# only they pull in. On a fresh board that is simply "what apt would download". On a board where the targets are already installed
# we recover the same set from apt itself: 'apt-get remove --autoremove' lists the targets plus the dependencies nothing else needs;
# we hide those from a COPY of dpkg's database and let apt compute the download list against that copy. Nothing on the board is changed.
#
# The bundle is only valid for the same Ubuntu release and CPU architecture (PYNQ v3.1: Ubuntu 22.04 "jammy", armhf).
set -euo pipefail
TARGETS="gcc-12 device-tree-compiler epiphany-browser geany micro pcmanfm isympy3 feh alsa-utils"
OUT="${1:-offline-out}"
NAME="panther-offline-bundle"
[ "$(id -u)" -eq 0 ] || exec sudo --preserve-env=HOME -- "$0" "$@"
OUT="$(mkdir -p "$OUT" && cd "$OUT" && pwd)"
WORK="$(mktemp -d)"; trap 'rm -rf "$WORK"' EXIT
DEST="$WORK/$NAME"; mkdir -p "$DEST" "$WORK/archives/partial" "$WORK/lists/partial"

apt-get update -qq
# 1. what does a stock image lack?  (targets + dependencies that only they need)
ADDED="$(apt-get -s remove --autoremove $TARGETS 2>/dev/null | awk '/^Remv/ {print $2}' | sed 's/:.*//' | sort -u || true)"
echo "packages this board has only because of $TARGETS: $(echo "$ADDED" | grep -c . || true)"
# 2. a copy of dpkg's database without them
python3 - "$WORK/status" $ADDED <<'PY'
import sys
out, drop = sys.argv[1], set(sys.argv[2:])
keep = []
for stanza in open("/var/lib/dpkg/status").read().split("\n\n"):
    name = next((l[9:].strip() for l in stanza.splitlines() if l.startswith("Package: ")), None)
    if name and name in drop:
        continue
    keep.append(stanza)
open(out, "w").write("\n\n".join(keep))
PY
# 3. let apt work out the download against that copy, into a scratch archive directory
APT_OPTS=(-o "Dir::State::status=$WORK/status" -o "Dir::Cache::archives=$WORK/archives" -o "Debug::NoLocking=1")
apt-get "${APT_OPTS[@]}" install --download-only -y --no-install-recommends $TARGETS
cp "$WORK"/archives/*.deb "$DEST/"
# 4. an index so apt can use the directory as a repository
( cd "$DEST" && apt-ftparchive packages . > Packages && gzip -9kf Packages )
( cd "$DEST" && sha256sum *.deb > SHA256SUMS )
cat > "$DEST/MANIFEST.txt" <<MAN
pynq-panther offline bundle
built:        $(date -u +%FT%TZ)
os:           $(. /etc/os-release; echo "$PRETTY_NAME") / $(dpkg --print-architecture)
kernel:       $(uname -r)
targets:      $TARGETS
packages:     $(ls "$DEST"/*.deb | wc -l)   size: $(du -sh "$DEST" | cut -f1)
licence note: these are unmodified Ubuntu packages (GPL/LGPL and others); sources: https://archive.ubuntu.com/ubuntu/pool/
MAN
tar -C "$WORK" -czf "$OUT/$NAME-$(uname -r | cut -d- -f1).tar.gz" "$NAME"
cat "$DEST/MANIFEST.txt"; echo; ls -l "$OUT"/*.tar.gz
