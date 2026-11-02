#!/bin/sh
# mkrelease.sh [--no-source] STAGING [OUT.zip]
#
#   --no-source   leave the source tarball out (README.txt then says the
#                 source will be published later)
#   FEEDBACK="..."  where testers should send feedback (shown in README.txt)
#
# Makes the zip people extract to the top of a blank FAT32 card:
#   kernel.elf, init.app, WIKIFONT.BIN, README.txt, every wiki folder found
#   in STAGING, and source/zimwiki-VERSION.tar.gz (the complete source,
#   which the GPL requires alongside the binaries).
# The card files come from card/ (prebuilt) or device/build/card/ when you
# have built them yourself; the wiki folders from STAGING (made by zimprep).
# Default OUT is WikireaderGen2.VERSION.zip in the current folder.
set -e
Z=$(cd "$(dirname "$0")/.." && pwd)
src=1
if [ "$1" = --no-source ]; then src=0; shift; fi
S=${1:?usage: tools/mkrelease.sh [--no-source] STAGING [OUT.zip]}
FEEDBACK=${FEEDBACK:-"reply in the thread where you found this release."}
V=$(sed -n 's/.*define ZW_VERSION "\(.*\)".*/\1/p' "$Z/src/zw.h")
OUT=${2:-$PWD/WikireaderGen2.$V.zip}
C=$Z/card
[ -f "$Z/device/build/card/init.app" ] && C=$Z/device/build/card
command -v zip >/dev/null || { echo "install zip (sudo apt install zip)"; exit 1; }
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
[ $src = 1 ] && mkdir -p "$T/source"
cp "$C/kernel.elf" "$C/init.app" "$C/WIKIFONT.BIN" "$Z/card/ZWSTATE.DAT" "$T/"
# placeholders zimprep did not make (wikis prepared with an older zimprep)
for d in "$S"/*/; do
	for f in "$d"*.IDX "$d"*.idx; do
		[ -f "$f" ] || continue
		b=${f%.*}
		for e in HIS EXT; do [ -f "$b.$e" ] || head -c 65536 /dev/zero > "$b.$e"; done
	done
done
if [ $src = 1 ]; then
	SRC="GPL v3. The complete source code, build instructions and the zimprep
tool are in source/zimwiki-$V.tar.gz in this package."
else
	SRC="GPL v3. The source code, build instructions and the zimprep tool
will be published soon."
fi
V="$V" SRC="$SRC" FB="$FEEDBACK" python3 -c '
import os, sys
s = open(sys.argv[1]).read()
s = s.replace("@VERSION@", os.environ["V"]).replace("@SOURCE@", os.environ["SRC"]).replace("@FEEDBACK@", os.environ["FB"])
open(sys.argv[2], "w").write(s)' "$Z/card/README.txt.in" "$T/README.txt"
n=0
for d in "$S"/*/; do
	[ -n "$(ls "$d" | grep -i -E '\.(idx|zim)$')" ] || continue
	cp -r "$d" "$T/"
	n=$((n + 1))
done
[ $n -gt 0 ] || echo "warning: no wiki folders in $S; the zip holds the software only"
[ $src = 1 ] && (cd "$Z/.." && tar --exclude="$(basename "$Z")/build" --exclude="$(basename "$Z")/device/build" \
	--exclude="$(basename "$Z")/.git" -czf "$T/source/zimwiki-$V.tar.gz" "$(basename "$Z")")
rm -f "$OUT"
(cd "$T" && zip -qr "$OUT" .)
echo "made $OUT ($(du -h "$OUT" | cut -f1)), version $V, $n wiki(s)"
