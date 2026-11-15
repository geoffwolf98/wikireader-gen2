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
		$B/zimprep --recluster 64 --level 3 --keep-fulltext --keep-images "$z" "$T/rc" > /dev/null 2>&1
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
        elif not listing:
            ok = ok and bytes(x.get_item().content) == bytes(y.get_item().content)
    if not ok:
        break
sys.exit(0 if ok else 1)
PY
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
	$B/zimprep --recluster 64 --level 3 "$P/pictest.zim" "$P/none" > /dev/null 2>&1
	[ ! -e "$P/none/WIKI.P00" ] && ok "no pictures unless asked" || bad "no pictures unless asked"
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
else
	echo "skip  pictures (needs ZIM_TESTS, libzim and Pillow for Python, libwebp)"
fi

echo "$pass passed, $fail failed"
[ $fail = 0 ]
