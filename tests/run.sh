#!/bin/sh
# Test suite.  Run from the top of the source tree after `make`:
#   sh tests/run.sh [ZIM...]
#
# Uses the openzim test files if ZIM_TESTS points at a checkout of
# github.com/openzim/zim-testing-suite (data/ folder), otherwise only the
# ZIMs given on the command line.  Optional extras, used when present:
#   python3 + libzim (pip install libzim)  reference check of every entry
#   dosfstools, mtools, sfdisk             card images for the app tests
#   device/build/init.elf + build/c33emu    run the WikiReader build too
set -e
B=build/host
T=build/test
mkdir -p "$T"
pass=0
fail=0
ok() { echo "ok    $*"; pass=$((pass + 1)); }
bad() { echo "FAIL  $*"; fail=$((fail + 1)); }

zims="$*"
if [ -z "$zims" ] && [ -n "$ZIM_TESTS" ]; then
	zims="$ZIM_TESTS/withns/small.zim $ZIM_TESTS/nons/small.zim
	      $ZIM_TESTS/withns/wikibooks_be_all_nopic_2017-02.zim
	      $ZIM_TESTS/nons/wikipedia_en_climate_change_mini_2024-06.zim"
fi
[ -n "$zims" ] || { echo "usage: sh tests/run.sh ZIM... (or set ZIM_TESTS)"; exit 2; }

have_libzim=0
python3 -c 'import libzim' 2>/dev/null && have_libzim=1

for z in $zims; do
	n=$(basename "$z")
	# 1. reader against libzim
	if [ $have_libzim = 1 ]; then
		python3 tests/refdump.py "$z" > "$T/ref.txt"
		$B/zimcat --dump "$z" > "$T/mine.txt" 2>/dev/null
		cmp -s "$T/ref.txt" "$T/mine.txt" && ok "reader matches libzim: $n" || bad "reader: $n"
	fi
	# 2. zimprep + index self check
	rm -rf "$T/prep"
	$B/zimprep "$z" "$T/prep" > /dev/null 2>&1 && $B/idxtest "$T/prep/WIKI.IDX" > /dev/null \
		&& ok "index: $n" || bad "index: $n"
	# 3. recluster, still a valid ZIM with identical content
	if [ $have_libzim = 1 ]; then
		rm -rf "$T/rc"
		$B/zimprep --recluster 64 --level 3 --keep-fulltext "$z" "$T/rc" > /dev/null 2>&1
		cat "$T"/rc/WIKI.Z* > "$T/rc.zim"
		python3 tests/refdump.py "$T/rc.zim" > "$T/rc.txt"
		python3 -c "from libzim.reader import Archive; import sys; sys.exit(0 if Archive('$T/rc.zim').check() else 1)" \
			&& cmp -s "$T/ref.txt" "$T/rc.txt" && ok "recluster: $n" || bad "recluster: $n"
		# default: search indexes left out, everything else identical
		rm -rf "$T/rc2"
		$B/zimprep --recluster 64 --level 3 "$z" "$T/rc2" > /dev/null 2>&1
		cat "$T"/rc2/WIKI.Z* > "$T/rc2.zim"
		python3 -c "
from libzim.reader import Archive
import sys
a, b = Archive('$z'), Archive('$T/rc2.zim')
ok = b.check() and a.entry_count == b.entry_count
for i in range(a.entry_count):
    x, y = a._get_entry_by_id(i), b._get_entry_by_id(i)
    if x.path != y.path: ok = False; break
    if x.is_redirect: continue
    p = x.path.split('/')[-2:] if '/' in x.path else [x.path]
    idx = x.path.startswith(('X/fulltext/', 'X/title/', 'fulltext/', 'title/', 'Z/'))
    if idx: ok = ok and y.get_item().size == 0
    elif bytes(x.get_item().content) != bytes(y.get_item().content): ok = False; break
sys.exit(0 if ok else 1)" && ok "recluster without search index: $n" || bad "recluster without search index: $n"
	fi
done

# 4. FAT layer and the whole app on a card image
if command -v mcopy >/dev/null && command -v mkfs.vfat >/dev/null && command -v sfdisk >/dev/null; then
	big=$(ls -S $zims | head -1)
	rm -rf "$T/card" && mkdir -p "$T/card"
	$B/zimprep --recluster 128 --level 3 --chunk-mb 1 "$big" "$T/card" > /dev/null 2>&1
	sh tests/mkcard.sh "$T/card.img" 64 "$T"/card/WIKI.*
	$B/fattest "$T/card.img" WIKI.Z00 "$T/card/WIKI.Z00" WIKI.IDX "$T/card/WIKI.IDX" > /dev/null \
		&& ok "FAT32 reads" || bad "FAT32 reads"
	cat > "$T/script.txt" <<'EOF'
expect search ""
type a
expect rows=10
tap 60 30
expect article
expect back=1
drag 120 180 120 40 300
button history
expect search "a"
button history
expect article
hold history 700
expect history
button history
expect article
button random
expect back=1
tap 15 200
expect back=0
button search
button search
type cli
tap 60 30
expect back=2
bounce history
expect search "cli"
bounce random
expect back=2
bounce history
expect search "cli"
bounce history
expect back=0
tap 15 200
expect search "cli"
restart
expect search "cli"
hold history 700
expect history n=4
tap 60 30
expect article
wait 300
tap 230 100
wait 300
tap 72 200
expect sections
tap 60 30
expect article
button history
expect article
EOF
	rm -rf "$T/state"
	$B/zimwiki-headless "$T/card.img" "$T/script.txt" "$T/state" > "$T/app.txt" \
		&& ok "app (PC build)" || { bad "app (PC build)"; cat "$T/app.txt"; }
	if [ -x build/c33emu ] && [ -f device/build/init.elf ]; then
		rm -rf "$T/state"
		# the emulator has no restart: run the part after it as a second
		# boot on the same saved state (checks that saves overwrite)
		sed '/^restart/,$d' "$T/script.txt" > "$T/script_emu.txt"
		sed '1,/^restart/d' "$T/script.txt" > "$T/script_emu2.txt"
		build/c33emu device/build/init.elf "$T/card.img" "$T/script_emu.txt" "$T/state" \
			> "$T/emu.txt" 2>&1 && ok "app (WikiReader build, emulated)" \
			|| { bad "app (WikiReader build, emulated)"; tail -20 "$T/emu.txt"; }
		build/c33emu device/build/init.elf "$T/card.img" "$T/script_emu2.txt" "$T/state" \
			> "$T/emu2.txt" 2>&1 && ok "state kept over power off (emulated)" \
			|| { bad "state kept over power off (emulated)"; tail -20 "$T/emu2.txt"; }
	fi
fi

echo "$pass passed, $fail failed"
[ $fail = 0 ]
