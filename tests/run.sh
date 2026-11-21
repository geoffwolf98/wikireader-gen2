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
		$B/zimprep --recluster 64 --level 3 --keep-fulltext --keep-images --no-slim "$z" "$T/rc" > /dev/null 2>&1
		cat "$T"/rc/WIKI.Z* > "$T/rc.zim"
		python3 tests/refdump.py "$T/rc.zim" > "$T/rc.txt"
		python3 -c "from libzim.reader import Archive; import sys; sys.exit(0 if Archive('$T/rc.zim').check() else 1)" \
			&& cmp -s "$T/ref.txt" "$T/rc.txt" && ok "recluster: $n" || bad "recluster: $n"
		# default: search indexes and pictures (and redirects to them) left
		# out, the rest renumbered; everything else identical, found by path
		rm -rf "$T/rc2"
		$B/zimprep --recluster 64 --level 3 "$z" "$T/rc2" > /dev/null 2>&1
		cat "$T"/rc2/WIKI.Z* > "$T/rc2.zim"
		python3 - "$z" "$T/rc2.zim" <<'PY' && ok "recluster without search index, pictures: $n" || bad "recluster without search index, pictures: $n"
import struct, sys
from libzim.reader import Archive
def dirents(path):
    f = open(path, 'rb'); h = f.read(80)
    n = struct.unpack('<I', h[24:28])[0]; up = struct.unpack('<Q', h[32:40])[0]
    f.seek(up); ptrs = struct.unpack('<%dQ' % n, f.read(8 * n)); out = []
    for p in ptrs:
        f.seek(p); d = f.read(1024); mime = struct.unpack('<H', d[:2])[0]
        o = 12 if mime == 0xffff else (8 if mime >= 0xfffd else 16)
        out.append((chr(d[3]), d[o:d.index(b'\0', o)].decode('utf-8', 'replace'), mime))
    return out
A, B = sys.argv[1], sys.argv[2]
a, b = Archive(A), Archive(B)
da, db = dirents(A), dirents(B)
ib = {d[:2]: i for i, d in enumerate(db)}
def picture(arch, i, d):
    if d[2] >= 0xfffd or d[0] == 'M':
        return False
    return arch._get_entry_by_id(i).get_item().mimetype.startswith('image/')
ok = b.check()
for i, d in enumerate(da):
    key = d[:2]
    x = a._get_entry_by_id(i)
    gone = picture(a, i, d)
    if d[2] == 0xffff:
        t = x.get_redirect_entry()
        gone = not t.is_redirect and t.get_item().mimetype.startswith('image/') and t.path.split('/')[0] != 'M'
    if gone:
        ok = ok and key not in ib
        continue
    if key not in ib:
        ok = False
        break
    y = b._get_entry_by_id(ib[key])
    if d[2] == 0xffff:
        ok = ok and y.is_redirect and x.get_redirect_entry().path == y.get_redirect_entry().path
    elif d[2] < 0xfffd:
        idx = key[1].startswith(('fulltext/', 'title/')) and key[0] == 'X' or key[0] == 'Z'
        listing = key[0] == 'X' and key[1].startswith('listing/')
        if idx:
            ok = ok and y.get_item().size == 0
        elif not listing and not x.get_item().mimetype.startswith('text/html'):
            ok = ok and bytes(x.get_item().content) == bytes(y.get_item().content)
    if not ok:
        break
sys.exit(0 if ok else 1)
PY
		# the slimmed articles convert as the originals did (every
		# combination of Footnotes, Citations and Pictures)
		$B/slimcheck --pair "$z" "$T/rc2.zim" > "$T/slim.txt" 2>&1 \
			&& ok "slimmed articles unchanged: $n" || { bad "slimmed articles unchanged: $n"; head -5 "$T/slim.txt"; }
		# a ZIM without a usable title list in its header (some newer
		# Kiwix ZIMs): zimprep rebuilds it, the same as the original
		python3 - "$z" "$T/notitles.zim" <<'PY'
import struct, sys
d = bytearray(open(sys.argv[1], 'rb').read())
n = struct.unpack('<I', d[24:28])[0]; tp = struct.unpack('<Q', d[40:48])[0]
d[tp:tp + 4 * n] = bytes(4 * n)
open(sys.argv[2], 'wb').write(d)
PY
		rm -rf "$T/rc3"
		$B/zimprep --recluster 64 --level 3 "$T/notitles.zim" "$T/rc3" > /dev/null 2>&1
		cat "$T"/rc3/WIKI.Z* > "$T/rc3.zim"
		python3 - "$T/rc2.zim" "$T/rc3.zim" <<'PY' && ok "title list rebuilt: $n" || bad "title list rebuilt: $n"
import struct, sys
from libzim.reader import Archive
# same titles in the same order, every entry once (the order of entries
# with equal titles is the writer's choice)
def keys(p):
    d = open(p, 'rb').read(); n = struct.unpack('<I', d[24:28])[0]
    tp = struct.unpack('<Q', d[40:48])[0]; up = struct.unpack('<Q', d[32:40])[0]
    tl = struct.unpack('<%dI' % n, d[tp:tp + 4 * n]); ptr = struct.unpack('<%dQ' % n, d[up:up + 8 * n])
    out = []
    for e in tl:
        q = ptr[e]; mime = struct.unpack('<H', d[q:q + 2])[0]
        o = 12 if mime == 0xffff else (8 if mime >= 0xfffd else 16)
        u = d.index(b'\0', q + o); t = d[u + 1:d.index(b'\0', u + 1)] or d[q + o:u]
        out.append((d[q + 3], t))
    return out, sorted(tl) == list(range(n))
ka, pa = keys(sys.argv[1]); kb, pb = keys(sys.argv[2])
sys.exit(0 if pa and pb and ka == kb and Archive(sys.argv[2]).check() else 1)
PY
		# the search index after renumbering: every title still leads
		# to the same article as in the index of the untouched ZIM
		python3 - "$z" "$T/rc2.zim" "$T/prep/WIKI.IDX" "$T/rc2/WIKI.IDX" "$B/idxtest" <<'PY' && ok "index after renumbering: $n" || bad "index after renumbering: $n"
import struct, subprocess, sys
def dirents(path):
    f = open(path, 'rb'); h = f.read(80)
    n = struct.unpack('<I', h[24:28])[0]; up = struct.unpack('<Q', h[32:40])[0]
    f.seek(up); ptrs = struct.unpack('<%dQ' % n, f.read(8 * n)); out = []
    for p in ptrs:
        f.seek(p); d = f.read(1024); mime = struct.unpack('<H', d[:2])[0]
        o = 12 if mime == 0xffff else (8 if mime >= 0xfffd else 16)
        out.append((chr(d[3]), d[o:d.index(b'\0', o)].decode('utf-8', 'replace')))
    return out
za, zb, xa, xb, tool = sys.argv[1:6]
def records(idx, zim):
    d = dirents(zim)
    out = subprocess.run([tool, idx, '', '100000000'], capture_output=True, text=True).stdout
    recs = []
    for line in out.splitlines():
        head, target = line.rsplit(' -> ', 1)
        recs.append((head.split(' ', 1)[1].strip() if ' ' in head.strip() else head, d[int(target)]))
    return recs
ra, rb = records(xa, za), records(xb, zb)
sys.exit(0 if ra and ra == rb else 1)
PY
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
hold random 700
expect calc
type 12+3x4=
expect result=24 done
tap 85 9
expect sci
type cs30)+r16)=
expect result=4.5 done sci
bounce random
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
hold random 700
expect sci
type a/2=
expect result=2.25 done
button random
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
	# power off with the calculator open over an article: back to the
	# article, and RANDOM leaves the calculator to it
	cat > "$T/calc1.txt" <<'EOF2'
type cli
tap 60 30
expect article
hold random 700
expect calc
type 1+1=
EOF2
	cat > "$T/calc2.txt" <<'EOF2'
expect article
hold random 700
expect calc
button random
expect article
EOF2
	{ cat "$T/calc1.txt"; echo restart; cat "$T/calc2.txt"; } > "$T/calc.txt"
	rm -rf "$T/state"
	$B/zimwiki-headless "$T/card.img" "$T/calc.txt" "$T/state" > "$T/calcapp.txt" 2>&1 \
		&& ok "calculator over power off (PC build)" \
		|| { bad "calculator over power off (PC build)"; cat "$T/calcapp.txt"; }
	# presses after the device has slept (its timer stops then): each
	# must count, not be taken for bounce of the last release
	cat > "$T/sleep.txt" <<'EOF2'
type cli
tap 60 30
wait 3000
expect article
hold random 700
expect calc
button random
expect article
hold random 700
expect calc
button history
expect article
button search
expect search
button search
expect search ""
EOF2
	rm -rf "$T/state"
	$B/zimwiki-headless "$T/card.img" "$T/sleep.txt" "$T/state" > "$T/sleepapp.txt" 2>&1 \
		&& ok "buttons after sleep (PC build, timer stopped)" \
		|| { bad "buttons after sleep (PC build, timer stopped)"; cat "$T/sleepapp.txt"; }
	if [ -x build/c33emu ] && [ -f device/build/init.elf ]; then
		rm -rf "$T/state"
		build/c33emu device/build/init.elf "$T/card.img" "$T/calc1.txt" "$T/state" > "$T/calcemu.txt" 2>&1 &&
		build/c33emu device/build/init.elf "$T/card.img" "$T/calc2.txt" "$T/state" >> "$T/calcemu.txt" 2>&1 \
			&& ok "calculator over power off (emulated)" \
			|| { bad "calculator over power off (emulated)"; tail -20 "$T/calcemu.txt"; }
	fi
fi

$B/calctest > "$T/calc.txt" 2>&1 && ok "calculator core" || { bad "calculator core"; cat "$T/calc.txt"; }

# a wiki on a badly fragmented card: its file map (here ~89 KB, more than
# the 64 KB read first) is saved and read back in full at the next start
CLIM="$ZIM_TESTS/nons/wikipedia_en_climate_change_mini_2024-06.zim"
if [ -n "$ZIM_TESTS" ] && [ -f "$CLIM" ]; then
	F=$T/frag
	rm -rf "$F" && mkdir -p "$F/fill" "$F/state/WIKI"
	python3 -c "
import os
for d in range(7):
    os.makedirs('$F/fill/D%d' % d)
    for i in range(10000): open('$F/fill/D%d/f%05d' % (d, i), 'wb').write(b'x' * 400)"
	truncate -s 34M "$F/card.img"
	printf 'label: dos\nstart=2048, type=c\n' | sfdisk -q "$F/card.img"
	mkfs.vfat -F 32 -s 1 --offset 2048 "$F/card.img" > /dev/null
	MTOOLS_SKIP_CHECK=1 mcopy -s -i "$F/card.img@@1M" "$F/fill" ::FILL > /dev/null 2>&1 || true
	python3 -c "
for d in range(7):
    for i in range(0, 10000, 2): print('::FILL/D%d/f%05d' % (d, i))" |
		MTOOLS_SKIP_CHECK=1 xargs -n 2000 mdel -i "$F/card.img@@1M" 2>/dev/null || true
	$B/zimprep --recluster --no-slim --keep-images "$CLIM" "$F/wiki" > /dev/null 2>&1
	MTOOLS_SKIP_CHECK=1 mmd -i "$F/card.img@@1M" ::WIKI
	MTOOLS_SKIP_CHECK=1 mcopy -i "$F/card.img@@1M" "$F/wiki/WIKI.Z00" "$F/wiki/WIKI.IDX" ::WIKI/
	cp "$F/wiki/WIKI.EXT" "$F/state/WIKI/WIKI.EXT"
	printf 'stats\n' > "$F/s.txt"
	$B/zimwiki-headless "$F/card.img" "$F/s.txt" "$F/state" > "$F/1.log" 2>&1
	$B/zimwiki-headless "$F/card.img" "$F/s.txt" "$F/state" > "$F/2.log" 2>&1
	s1=$(grep -o '[0-9]* sectors' "$F/1.log" | head -1 | cut -d' ' -f1)
	s2=$(grep -o '[0-9]* sectors' "$F/2.log" | head -1 | cut -d' ' -f1)
	ext=$(wc -c < "$F/state/WIKI/WIKI.EXT")
	[ "$ext" -gt 65536 ] && [ "$s2" -lt "$s1" ] \
		&& ok "file map over 64 KB saved and reused" || bad "file map over 64 KB saved and reused (map $ext bytes, $s1 then $s2 sectors)"
	[ "$(wc -c < "$F/wiki/WIKI.EXT")" = 262144 ] && ok "zimprep makes a 256 KB file map" || bad "zimprep makes a 256 KB file map"
fi

# slimming: awkward HTML (ticks, pre, maths, notes, boxes, odd markup)
# converts the same slimmed
if python3 -c "import libzim" 2>/dev/null; then
	python3 tests/mkslimzim.py "$T/slimtest.zim" > /dev/null 2>&1
	$B/slimcheck "$T/slimtest.zim" > "$T/slimtest.txt" 2>&1 \
		&& ok "slimming: awkward HTML unchanged" || { bad "slimming: awkward HTML unchanged"; cat "$T/slimtest.txt"; }
fi

# pictures (zimprep --pictures): a maxi style test ZIM made from the
# climate change ZIM's pictures (needs libzim and Pillow for Python, and a
# zimprep built with libwebp)
CLIMATE=
[ -n "$ZIM_TESTS" ] && CLIMATE="$ZIM_TESTS/nons/wikipedia_en_climate_change_mini_2024-06.zim"
if [ -f "$CLIMATE" ] && python3 -c "import libzim, PIL" 2>/dev/null &&
   $B/zimprep --pictures all 2>&1 | grep -q "usage"; then
	P=$T/pic
	rm -rf "$P"
	mkdir -p "$P"
	python3 tests/mkpiczim.py "$CLIMATE" "$P/pictest.zim" > /dev/null 2>&1
	# the pictures each article should get
	cat > "$P/want.py" <<'PY'
import struct, sys, glob
from libzim.reader import Archive
base, zim, mode = sys.argv[1], sys.argv[2], sys.argv[3]
want = {'Cattle': 2, 'Gujarat_work': 1, 'Bamboo': 1, 'Lamb': 2, 'Shared': 1,
        'Wind/Power': 1, 'Long_page': 1}
if mode == 'lead':
    want = {k: 1 for k in want}
d = b''.join(open(p, 'rb').read() for p in sorted(glob.glob(base + '.P[0-9][0-9]')))
a = Archive(zim)
n = struct.unpack('<I', d[8:12])[0]
got = {}
offs = {}
for i in range(n):
    e, off = struct.unpack('<IQ', d[64 + 12 * i:76 + 12 * i])
    cnt = d[off]
    got[a._get_entry_by_id(e).path] = cnt
    offs[a._get_entry_by_id(e).path] = [struct.unpack('<Q', d[off + 12 + 16 * k:off + 20 + 16 * k])[0]
                                        for k in range(cnt)]
ok = got == want
if mode == 'all':
    ok = ok and offs['Shared'][0] == offs['Cattle'][0]   # one bitmap, shared
if not ok:
    print(got)
sys.exit(0 if ok else 1)
PY
	for m in all lead; do
		rm -rf "$P/$m"
		$B/zimprep --recluster 64 --level 3 --pictures $m "$P/pictest.zim" "$P/$m" > "$P/$m.log" 2>&1
		cat "$P/$m"/WIKI.Z* > "$P/$m.zim"
		python3 tests/picdump.py "$P/$m/WIKI" --check "$P/$m.zim" > /dev/null &&
		python3 "$P/want.py" "$P/$m/WIKI" "$P/$m.zim" $m \
			&& ok "pictures made: $m" || { bad "pictures made: $m"; cat "$P/$m.log"; }
	done
	rm -rf "$P/none"
	$B/zimprep --recluster 64 --level 3 "$P/pictest.zim" "$P/none" > "$P/none.log" 2>&1
	[ ! -e "$P/none/WIKI.P00" ] && ok "no pictures unless asked" || bad "no pictures unless asked"
	# video, audio and subtitles are left out too (350 KB of them in the
	# test ZIM; the device cannot play them)
	grep -q "left out 3 video, audio and subtitle files" "$P/none.log" &&
	! grep -q "Cattle_moo" "$P/none/WIKI.Z00" && [ "$(stat -c %s "$P/none/WIKI.Z00")" -lt 300000 ] \
		&& ok "video and audio left out" || { bad "video and audio left out"; cat "$P/none.log"; }
	# zimprep --types: what a ZIM holds and what --recluster keeps
	$B/zimprep --types "$P/pictest.zim" > "$P/types.txt" 2>/dev/null &&
	grep -q "^video/webm .* 1 .*300 KB  left out (cannot be played)" "$P/types.txt" &&
	grep -q "^text/html .* 10 .* kept: articles" "$P/types.txt" \
		&& ok "zimprep --types" || { bad "zimprep --types"; cat "$P/types.txt"; }
	# --add-pictures: the same files as making them with the wiki
	rm -rf "$P/add" && cp -r "$P/none" "$P/add"
	$B/zimprep --threads 2 --add-pictures all "$P/pictest.zim" "$P/add" > "$P/add.log" 2>&1 &&
	cmp -s "$P/add/WIKI.P00" "$P/all/WIKI.P00" && cmp -s "$P/add/WIKI.Z00" "$P/none/WIKI.Z00" \
		&& ok "--add-pictures to a prepared wiki" || { bad "--add-pictures to a prepared wiki"; cat "$P/add.log"; }
	# ... on all CPUs by default (it once used one)
	rm -rf "$P/add3" && cp -r "$P/none" "$P/add3"
	$B/zimprep --add-pictures lead "$P/pictest.zim" "$P/add3" 2>&1 | grep -q "on $(nproc) thread" \
		&& ok "--add-pictures uses all CPUs" || bad "--add-pictures uses all CPUs"
	# ... refused for a wiki made from another ZIM
	! $B/zimprep --add-pictures lead "$CLIMATE" "$P/add" > "$P/add2.log" 2>&1 &&
	grep -q "another ZIM" "$P/add2.log" && cmp -s "$P/add/WIKI.P00" "$P/all/WIKI.P00" \
		&& ok "--add-pictures refuses another ZIM's wiki" || { bad "--add-pictures refuses another ZIM's wiki"; cat "$P/add2.log"; }
	# pictures twice (originals in .Z and bitmaps in .P): refused
	! $B/zimprep --recluster 64 --keep-images --pictures lead "$P/pictest.zim" "$P/kp" > "$P/kp.log" 2>&1 &&
	[ ! -e "$P/kp/WIKI.Z00" ] \
		&& ok "--keep-images with --pictures refused" || { bad "--keep-images with --pictures refused"; cat "$P/kp.log"; }
	# a run without pictures removes an older run's picture files
	cp "$P/all/WIKI.P00" "$P/none/"
	$B/zimprep --recluster 64 --level 3 "$P/pictest.zim" "$P/none" > /dev/null 2>&1
	[ ! -e "$P/none/WIKI.P00" ] && ok "old picture files removed" || bad "old picture files removed"
	# the same files whatever the number of threads
	rm -rf "$P/t1"
	$B/zimprep --recluster 64 --level 3 --threads 1 --pictures all "$P/pictest.zim" "$P/t1" > /dev/null 2>&1
	cmp -s "$P/t1/WIKI.P00" "$P/all/WIKI.P00" && cmp -s "$P/t1/WIKI.Z00" "$P/all/WIKI.Z00" \
		&& ok "same output on one thread" || bad "same output on one thread"
	# Atkinson by default; Floyd-Steinberg on request (bigger files)
	rm -rf "$P/fs"
	$B/zimprep --recluster 64 --level 3 --dither fs --pictures all "$P/pictest.zim" "$P/fs" > /dev/null 2>&1
	[ "$(wc -c < "$P/all/WIKI.P00")" -lt "$(wc -c < "$P/fs/WIKI.P00")" ] \
		&& ok "Atkinson pictures by default (smaller than --dither fs)" || bad "Atkinson pictures by default (smaller than --dither fs)"
	# compressed bitmaps (version 2)
	python3 tests/picdump.py "$P/all/WIKI" | grep -q "version 2, [1-9][0-9]* compressed" \
		&& ok "picture bitmaps compressed" || bad "picture bitmaps compressed"
	# old style ZIM (A/ and I/ namespaces): the home page's pictures
	if [ -f "$ZIM_TESTS/withns/wikipedia_en_climate_change_mini_2024-06.zim" ]; then
		rm -rf "$P/old"
		$B/zimprep --recluster 64 --level 3 --pictures all \
			"$ZIM_TESTS/withns/wikipedia_en_climate_change_mini_2024-06.zim" "$P/old" > /dev/null 2>&1
		cat "$P/old"/WIKI.Z* > "$P/old.zim"
		python3 tests/picdump.py "$P/old/WIKI" --check "$P/old.zim" | grep -q "^1 articles, 32 picture" \
			&& ok "pictures made: old style ZIM" || bad "pictures made: old style ZIM"
	fi
	# on the WikiReader: each article's pictures, the switch (keeping the
	# place), a redirect
	cat > "$P/app.txt" <<'EOF2'
type cattle
tap 60 30
expect pics=2
drag 120 200 120 20 300
drag 120 200 120 20 300
wait 1500
button search
button search
expect search ""
tap 180 104
button history
expect pics=2 hidden
button search
button search
tap 180 104
button history
expect pics=2
button search
button search
type gujarat
tap 60 30
expect pics=1
button search
button search
type bamboo
tap 60 30
expect pics=1
button search
button search
type lamb
tap 60 30
expect pics=2
button search
button search
type wind power
tap 60 30
expect pics=1
button search
button search
type long page
tap 60 30
expect pics=1
drag 120 200 120 0 30
wait 3000
button search
button search
type plain
tap 60 30
expect pics=0
button search
button search
type cow
tap 60 30
expect "Cattle"
expect pics=2
EOF2
	mkdir -p "$P/card/WIKI"
	cp "$P/all"/WIKI.* "$P/card/WIKI/"
	sh tests/mkcard.sh "$P/card.img" 32 > /dev/null 2>&1
	MTOOLS_SKIP_CHECK=1 mcopy -s -i "$P/card.img@@1M" "$P/card/WIKI" ::WIKI
	rm -rf "$P/state"
	$B/zimwiki-headless "$P/card.img" "$P/app.txt" "$P/state" > "$P/app.log" 2>&1 \
		&& ok "pictures in articles (PC build)" || { bad "pictures in articles (PC build)"; grep FAIL "$P/app.log"; }
	if [ -x build/c33emu ] && [ -f device/build/init.elf ]; then
		rm -rf "$P/state"
		build/c33emu device/build/init.elf "$P/card.img" "$P/app.txt" "$P/state" > "$P/emu.log" 2>&1 \
			&& ok "pictures in articles (emulated)" || { bad "pictures in articles (emulated)"; grep FAIL "$P/emu.log"; }
	fi
	# scrolling never waits for the card: while the finger is down a
	# picture not yet read is a frame, read once the page stops; and a
	# slow frame (a stall) never turns into a fling
	{
		printf 'type long page\ntap 60 30\n'
		i=0; while [ $i -lt 20 ]; do
			printf 'down 120 200\nmove 120 160\nmove 120 120\nmove 120 80\nmove 120 40\n'
			[ $i = 19 ] && printf 'describe\n'
			printf 'up 120 40\n'
			i=$((i + 1))
		done
		printf 'wait 3000\ndescribe\n'
	} > "$P/swipe.txt"
	rm -rf "$P/state"
	ZW_SECTOR_US=8000 $B/zimwiki-headless "$P/card.img" "$P/swipe.txt" "$P/state" > "$P/swipe.log" 2>&1
	grep -q "describe.*picwait" "$P/swipe.log" && tail -2 "$P/swipe.log" | grep "describe" | grep -v -q picwait \
		&& ok "pictures read after scrolling, not during" || { bad "pictures read after scrolling, not during"; tail -5 "$P/swipe.log"; }
	printf 'type long page\ntap 60 30\nslowdraw %s\ndown 120 200\nmove 120 180\nmove 120 160\nmove 120 140\nmove 120 120\nup 120 120\nslowdraw 0\nwait 2000\nexpect scroll=%s/\n' \
		0 349 > "$P/fling1.txt"
	printf 'type long page\ntap 60 30\nslowdraw %s\ndown 120 200\nmove 120 180\nmove 120 160\nmove 120 140\nmove 120 120\nup 120 120\nslowdraw 0\nwait 2000\nexpect scroll=%s/\n' \
		80 80 > "$P/fling2.txt"
	rm -rf "$P/state"
	$B/zimwiki-headless "$P/card.img" "$P/fling1.txt" "$P/state" > "$P/fling.log" 2>&1 &&
	{ rm -rf "$P/state"; $B/zimwiki-headless "$P/card.img" "$P/fling2.txt" "$P/state" >> "$P/fling.log" 2>&1; } \
		&& ok "no fling after a stall (overscroll)" || { bad "no fling after a stall (overscroll)"; grep FAIL "$P/fling.log"; }
	# safe mode: SEARCH held at power on forgets the page it would reopen
	# (and the settings), ignores that SEARCH press, keeps working after
	printf 'type cattle\ntap 60 30\nexpect pics=2\nwait 3000\n' > "$P/sm1.txt"
	printf 'expect "Safe mode"\nbutton search\nexpect "Safe mode"\ntap 120 100\nexpect search ""\n' > "$P/sm2.txt"
	printf 'expect search ""\n' > "$P/sm3.txt"
	for R in "$B/zimwiki-headless" "build/c33emu device/build/init.elf"; do
		[ -x "${R%% *}" ] || continue
		rm -rf "$P/sms"
		$R "$P/card.img" "$P/sm1.txt" "$P/sms" > "$P/sm.log" 2>&1 &&
		ZW_HELD=search $R "$P/card.img" "$P/sm2.txt" "$P/sms" >> "$P/sm.log" 2>&1 &&
		$R "$P/card.img" "$P/sm3.txt" "$P/sms" >> "$P/sm.log" 2>&1 \
			&& ok "safe mode ($(basename "${R%% *}"))" || { bad "safe mode ($(basename "${R%% *}"))"; grep FAIL "$P/sm.log"; }
	done
	# find in article: Find in the toolbar, search as you type (case and
	# accents ignored), drag to put the keyboard away, step with the
	# arrows, tap the page to close; a caption hidden with Pictures off
	# does not count; a button closes it
	cat > "$P/find.txt" <<'EOF2'
type long page
tap 60 30
tap 120 100
tap 220 196
expect find="" 0/0 kb
type Paragraph 3
expect find="paragraph 3" 1/12 kb
drag 120 60 120 30 200
expect find="paragraph 3" 1/12
expectnot kb
tap 198 196
tap 198 196
expect 3/12
tap 114 196
expect 2/12
tap 120 60
expectnot find=
tap 120 100
tap 220 196
expect find="paragraph 3"
button search
expectnot find=
button search
type cattle
tap 60 30
tap 120 100
tap 220 196
type fleckvieh
expect find="fleckvieh" 1/2 kb
type x
expect 0/0
button search
button search
tap 180 104
type cattle
tap 60 30
tap 120 100
tap 220 196
expect find="fleckviehx" 0/0 kb
tap 228 166
expect find="fleckvieh" 1/1 kb
EOF2
	for R in "$B/zimwiki-headless" "build/c33emu device/build/init.elf"; do
		[ -x "${R%% *}" ] || continue
		rm -rf "$P/state"
		$R "$P/card.img" "$P/find.txt" "$P/state" > "$P/find.log" 2>&1 \
			&& ok "find in article ($(basename "${R%% *}"))" || { bad "find in article ($(basename "${R%% *}"))"; grep FAIL "$P/find.log"; }
	done
	# favourites: the star at the top of the toolbar, newest first,
	# starring again moves it to the top, the HISTORY list's tab is
	# remembered, and a favourite is found by its address when the wiki
	# has been rebuilt (entry numbers changed); gone: a message
	cat > "$P/fav0.txt" <<'EOF2'
type cattle
tap 60 30
tap 120 100
tap 120 8
expect article "Cattle"
expectnot fav
hold history 700
tap 100 10
expect history n=1
wait 3000
EOF2
	cat > "$P/fav1.txt" <<'EOF2'
button search
button search
type cattle
tap 60 30
tap 120 100
tap 120 8
expect article "Cattle"
expect fav
button search
button search
type plain
tap 60 30
expect article "Plain"
tap 120 100
tap 120 8
expect fav
hold history 700
expect history n=2 first="Plain"
tap 100 10
expect favourites n=2 first="Plain"
wait 3000
EOF2
	cat > "$P/fav2.txt" <<'EOF2'
hold history 700
expect favourites n=2 first="Plain"
tap 60 49
expect article "Cattle"
expect fav
tap 120 100
tap 120 8
expectnot fav
tap 120 8
expect fav
hold history 700
expect favourites n=2 first="Cattle"
tap 30 10
expect history
tap 100 10
tap 60 49
expect article "Plain"
button search
button search
type huge page
tap 60 30
tap 120 100
tap 120 8
expect fav
drag 120 180 120 30 600
wait 1000
expectnot scroll=0/
button search
hold history 700
expect favourites n=3 first="Huge page"
tap 60 30
expect article "Huge page"
expectnot scroll=0/
wait 3000
EOF2
	cat > "$P/fav3.txt" <<'EOF2'
hold history 700
expect favourites n=3 first="Huge page"
tap 60 49
expect article "Cattle"
expect fav
hold history 700
tap 60 68
expect message "Favourites"
tap 120 100
expect favourites n=2 first="Huge page"
tap 30 10
expect history n=5 first="Cattle"
tap 60 49
expect article "Huge page"
hold history 700
expect history n=5 first="Huge page"
tap 60 87
expect message "History"
tap 120 100
expect history n=4 first="Huge page"
EOF2
	for R in "$B/zimwiki-headless" "build/c33emu device/build/init.elf"; do
		[ -x "${R%% *}" ] || continue
		# no WIKI.FAV: no star, no Favourites tab
		rm -rf "$P/favs"
		if $R "$P/card.img" "$P/fav0.txt" "$P/favs" > "$P/fav.log" 2>&1 &&
		   [ ! -e "$P/favs/WIKI/WIKI.FAV" ] &&
		   head -c 32768 /dev/zero > "$P/favs/WIKI/WIKI.FAV" &&
		   $R "$P/card.img" "$P/fav1.txt" "$P/favs" >> "$P/fav.log" 2>&1 &&
		   $R "$P/card.img" "$P/fav2.txt" "$P/favs" >> "$P/fav.log" 2>&1 &&
		   python3 - "$P/favs/WIKI/WIKI.FAV" <<'EOF2' &&
# a rebuilt wiki: every entry number now wrong, and Plain gone from it
import struct, sys
f = sys.argv[1]; b = bytearray(open(f, 'rb').read())
n = struct.unpack_from('<I', b, 4)[0]; p = 8
for i in range(n):
    struct.pack_into('<I', b, p, struct.unpack_from('<I', b, p)[0] + 1)
    tl = b[p + 5]; ul = b[p + 6 + tl]
    if b[p + 6:p + 6 + tl] == b'Plain':
        b[p + 7 + tl] = ord('Q')
    p += 7 + tl + ul
open(f, 'wb').write(b)
h = f[:-4] + '.HIS'; b = bytearray(open(h, 'rb').read())
n = struct.unpack_from('<I', b, 4)[0]; p = 8; titles = []
for i in range(n):
    struct.pack_into('<I', b, p, struct.unpack_from('<I', b, p)[0] + 1)
    tl = b[p + 8]; titles.append(bytes(b[p + 9:p + 9 + tl])); p += 9 + tl
assert b[p:p + 4] == b'WZSA'; p += 4
for i in range(n):
    ul = b[p + 1]
    if titles[i] == b'Plain':
        b[p + 2] = ord('Q')
    p += 2 + ul
open(h, 'wb').write(b)
EOF2
		   $R "$P/card.img" "$P/fav3.txt" "$P/favs" >> "$P/fav.log" 2>&1; then
			ok "favourites ($(basename "${R%% *}"))"
		else
			bad "favourites ($(basename "${R%% *}"))"; grep FAIL "$P/fav.log"
		fi
	done
	# tap to stop: a tap while a page opens stops it and leaves you where
	# you were (the old article kept, nothing added to history); at start
	# it gives an empty search and the next start does not try the page
	# again.  On the PC build "stopat N" taps during stage N (1 unpacking,
	# 2 reading, 4 WIKIFONT.BIN: that load is never stopped, the tap
	# stops the opening just after it, and the font stays loaded)
	cat > "$P/stop1.txt" <<'EOF2'
type huge page
stopat 1
tap 60 30
expect search "huge page"
button search
type cattle
tap 60 30
expect article "Cattle"
button search
button search
type huge page
stopat 2
tap 60 30
expect search "huge page"
stopat -1
button history
expect article "Cattle"
button search
button search
type kanji
stopat 4
tap 60 30
expect search "kanji"
tap 60 30
expect article "Kanji"
EOF2
	printf 'type huge page\ntap 60 30\nexpect article "Huge page"\nwait 3000\n' > "$P/stop2.txt"
	printf 'expect search ""\nwait 3000\n' > "$P/stop3.txt"
	rm -rf "$P/stops"; mkdir -p "$P/stops"; cp card/WIKIFONT.BIN "$P/stops/"
	if $B/zimwiki-headless "$P/card.img" "$P/stop1.txt" "$P/stops" > "$P/stop.log" 2>&1 &&
	   [ "$(grep -c 'WIKIFONT.BIN loaded' "$P/stop.log")" = 1 ] && grep -q "open_entry .* stopped" "$P/stop.log" &&
	   ! grep -q '"Huge page"' "$P/stops/"*.HIS 2>/dev/null &&
	   rm -f "$P/stops/ZWSTATE.DAT" && $B/zimwiki-headless "$P/card.img" "$P/stop2.txt" "$P/stops" >> "$P/stop.log" 2>&1 &&
	   ZW_STOP_STAGE=2 $B/zimwiki-headless "$P/card.img" "$P/stop3.txt" "$P/stops" >> "$P/stop.log" 2>&1 &&
	   $B/zimwiki-headless "$P/card.img" "$P/stop3.txt" "$P/stops" >> "$P/stop.log" 2>&1; then
		ok "tap to stop (zimwiki-headless)"
	else
		bad "tap to stop (zimwiki-headless)"; grep -E "FAIL|WIKIFONT|stopped" "$P/stop.log"
	fi
	# the emulated device, in emulated time: a second tap 0.5 s after the
	# one that opens a 390 KB page; and a tap 0.7 s after power on
	printf 'type huge page\ntap2 60 30 500 240 150\nexpect search "huge page"\nwait 2000\nexpect search "huge page"\n' > "$P/stope.txt"
	if [ -x build/c33emu ]; then
		rm -rf "$P/stops"
		if build/c33emu device/build/init.elf "$P/card.img" "$P/stope.txt" "$P/stops" > "$P/stope.log" 2>&1 &&
		   rm -f "$P/stops/ZWSTATE.DAT" && build/c33emu device/build/init.elf "$P/card.img" "$P/stop2.txt" "$P/stops" >> "$P/stope.log" 2>&1 &&
		   ZW_EARLY_TAP=700 build/c33emu device/build/init.elf "$P/card.img" "$P/stop3.txt" "$P/stops" >> "$P/stope.log" 2>&1 &&
		   build/c33emu device/build/init.elf "$P/card.img" "$P/stop3.txt" "$P/stops" >> "$P/stope.log" 2>&1; then
			ok "tap to stop (c33emu)"
		else
			bad "tap to stop (c33emu)"; grep -E "FAIL" "$P/stope.log"
		fi
	fi
	# pictures made from another ZIM are not used
	mkdir -p "$P/card2/WIKI"
	cp "$P/none"/WIKI.* "$P/card2/WIKI/"
	cp "$P/old/WIKI.P00" "$P/card2/WIKI/"
	sh tests/mkcard.sh "$P/card2.img" 32 > /dev/null 2>&1
	MTOOLS_SKIP_CHECK=1 mcopy -s -i "$P/card2.img@@1M" "$P/card2/WIKI" ::WIKI
	printf 'type cattle\ntap 60 30\nexpect pics=0\n' > "$P/app2.txt"
	rm -rf "$P/state"
	$B/zimwiki-headless "$P/card2.img" "$P/app2.txt" "$P/state" > "$P/app2.log" 2>&1 \
		&& ok "pictures of another ZIM ignored" || { bad "pictures of another ZIM ignored"; cat "$P/app2.log"; }
	# ... and those of another build of the same ZIM (other numbering:
	# made with --keep-images), which version 1 files could not tell
	rm -rf "$P/kp2"
	$B/zimprep --recluster 64 --level 3 --keep-images "$P/pictest.zim" "$P/kp2" > /dev/null 2>&1
	$B/zimprep --add-pictures all "$P/pictest.zim" "$P/kp2" > /dev/null 2>&1
	mkdir -p "$P/card3/WIKI"
	cp "$P/none"/WIKI.* "$P/card3/WIKI/"
	cp "$P/kp2/WIKI.P00" "$P/card3/WIKI/"
	sh tests/mkcard.sh "$P/card3.img" 32 > /dev/null 2>&1
	MTOOLS_SKIP_CHECK=1 mcopy -s -i "$P/card3.img@@1M" "$P/card3/WIKI" ::WIKI
	rm -rf "$P/state"
	$B/zimwiki-headless "$P/card3.img" "$P/app2.txt" "$P/state" > "$P/app3.log" 2>&1 \
		&& ok "pictures of another build ignored" || { bad "pictures of another build ignored"; cat "$P/app3.log"; }
else
	echo "skip  pictures (needs ZIM_TESTS, libzim and Pillow for Python, libwebp)"
fi

# a title index past the 4 GiB limit is refused before any slow step
# (ZW_IDX_LIMIT lowers the limit; the size check is exact)
if [ -f build/test/rc2.zim ]; then
	rm -rf build/test/idxlim
	if ZW_IDX_LIMIT=530963 $B/zimprep --recluster 64 build/test/rc2.zim build/test/idxlim > build/test/idxlim.log 2>&1; then
		bad "index limit"
	elif grep -q "limited to 4 GiB" build/test/idxlim.log && ! ls build/test/idxlim/*.Z* > /dev/null 2>&1 &&
	     ZW_IDX_LIMIT=530964 $B/zimprep build/test/rc2.zim build/test/idxlim > /dev/null 2>&1; then
		ok "index limit"
	else
		bad "index limit"; cat build/test/idxlim.log
	fi
fi

echo "$pass passed, $fail failed"
[ $fail = 0 ]
