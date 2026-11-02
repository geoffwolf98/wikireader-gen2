#!/bin/sh
# sim.sh STAGING [WIKI...]
#
# Runs the PC simulator on a card image made from a staging folder (the
# folder you copy to the card: kernel.elf, init.app, WIKIFONT.BIN and one
# folder per wiki).  The image is built once and reused until the list of
# wikis changes; it is a sparse file, so it only takes the space of its
# contents.  WIKI defaults to every wiki folder in STAGING.
#
#   tools/sim.sh ~/card                 all wikis in ~/card
#   tools/sim.sh ~/card MATHS           just MATHS (quick to build)
#   SCALE=2 tools/sim.sh ~/card         smaller window (default 3)
#
# Mouse = finger, F1 = SEARCH, F2 = HISTORY (tap: back, hold: list),
# F3 = RANDOM, typing goes to the search box, Esc quits.
set -e
Z=$(cd "$(dirname "$0")/.." && pwd)
S=${1:?usage: tools/sim.sh STAGING [WIKI...]}
shift
IMG=$Z/build/sim.img
STATE=$Z/build/sim-state
if [ $# -gt 0 ]; then
	WIKIS="$*"
else
	WIKIS=$(cd "$S" && for d in */; do [ -n "$(ls "$d" | grep -i -E '\.(idx|zim)$')" ] && printf '%s ' "${d%/}"; done)
fi
[ -n "$WIKIS" ] || { echo "no wiki folders in $S"; exit 1; }
[ -x "$Z/build/host/zimwiki-sdl" ] || { echo "build the simulator first: make (needs libsdl2-dev)"; exit 1; }
export MTOOLS_SKIP_CHECK=1
if [ ! -f "$IMG" ] || [ "$(cat "$IMG.wikis" 2>/dev/null)" != "$WIKIS" ]; then
	mb=$(du -smc $(for w in $WIKIS; do echo "$S/$w"; done) | tail -1 | cut -f1)
	mb=$((mb + mb / 20 + 64))
	echo "building $IMG ($mb MB) with: $WIKIS"
	rm -f "$IMG.wikis"
	sh "$Z/tests/mkcard.sh" "$IMG" "$mb"
	for w in $WIKIS; do
		echo "copying $w..."
		mcopy -s -i "$IMG@@1M" "$S/$w" "::$w"
	done
	[ -f "$S/WIKIFONT.BIN" ] && mcopy -i "$IMG@@1M" "$S/WIKIFONT.BIN" ::
	# the simulator reads saved state (and the CJK font) from its state folder
	mkdir -p "$STATE"
	[ -f "$S/WIKIFONT.BIN" ] && cp "$S/WIKIFONT.BIN" "$STATE/"
	echo "$WIKIS" > "$IMG.wikis"
fi
# the simulator reads the fallback font from its state folder first:
# always use the one from this source, so a new font is never missed
mkdir -p "$STATE"
cp "$Z/WIKIFONT.BIN" "$STATE/"
exec "$Z/build/host/zimwiki-sdl" "$IMG" "$STATE" "${SCALE:-3}"
