#!/bin/sh
# mkroot.sh OUTDIR [ITEM]
#
# Makes the software-only release in OUTDIR (emptied first): kernel.elf,
# init.app, WIKIFONT.BIN, a blank ZWSTATE.DAT, README.txt and
# source/zimwiki-VERSION.tar.gz.  This is what a new card needs besides
# the wiki folders, and what an existing card needs to be updated
# (init.app etc.; see README.txt).
#
# With ITEM it also uploads OUTDIR to that archive.org item with the ia
# tool, as a folder of the same name (like the main release item, so the
# card files stay apart from archive.org's own files), then removes
# older source tarballs from the item.
#
#   tools/mkroot.sh ~/release/WikireaderGen2_Root
#   tools/mkroot.sh ~/release/WikireaderGen2_Root WikireaderGen2_Root
set -e
Z=$(cd "$(dirname "$0")/.." && pwd)
R=${1:?usage: tools/mkroot.sh OUTDIR [ITEM]}
ITEM=$2
V=$(sed -n 's/.*define ZW_VERSION "\(.*\)".*/\1/p' "$Z/src/zw.h")
command -v unzip >/dev/null || { echo "install unzip (sudo apt install unzip)"; exit 1; }
E=$(mktemp -d)
trap 'rm -rf "$E"' EXIT
mkdir "$E/none"
sh "$Z/tools/mkrelease.sh" "$E/none" "$E/root.zip" 2>&1 | grep -v "no wiki folders" || true
rm -rf "$R"
mkdir -p "$R"
(cd "$R" && unzip -q "$E/root.zip")
echo "made $R, version $V:"
(cd "$R" && ls -l . source)
[ -n "$ITEM" ] || exit 0
command -v ia >/dev/null || { echo "install the archive.org tool: pip install internetarchive"; exit 1; }
cd "$(dirname "$R")"
ia -l upload "$ITEM" "$(basename "$R")" \
	--metadata="mediatype:data" --metadata="collection:opensource_media" \
	--checksum --retries 100 --sleep 60 --no-derive
old=$(ia list "$ITEM" | grep '/source/zimwiki-.*\.tar\.gz$' | grep -v "zimwiki-$V\.tar\.gz" || true)
if [ -n "$old" ]; then
	echo "removing older source: $old"
	ia delete "$ITEM" $old
fi
ia list "$ITEM" --columns=name,size,mtime
