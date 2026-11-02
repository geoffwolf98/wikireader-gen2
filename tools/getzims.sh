#!/bin/sh
# getzims.sh [--get] [DIR] [PROJECT...]
#
# Finds the newest English Kiwix ZIM of each project and shows its name and
# size; with --get it also downloads them into DIR (default: current folder).
# Uses aria2c (several connections and mirrors, via the .meta4 metalink)
# when installed, otherwise wget.  Downloads resume if interrupted.
#
# PROJECT is one of the names below; default is all of them.
#   wikipedia     full English Wikipedia, no pictures (about 50 GB)
#   mathematics   the Mathematics selection of Wikipedia (about 1 GB)
#   wiktionary wikivoyage wikibooks wikiquote wikisource wikiversity
#
# Examples:
#   tools/getzims.sh                          list everything
#   tools/getzims.sh --get ~/zims mathematics download the maths wiki
set -e
K=https://download.kiwix.org/zim
get=0
if [ "$1" = --get ]; then get=1; shift; fi
dir=.
if [ -n "$1" ] && [ -d "$1" ]; then dir=$1; shift; fi
projects=${*:-"wikipedia mathematics wiktionary wikivoyage wikibooks wikiquote wikisource wikiversity"}

newest() {	# folder pattern...
	list=$(curl -sL "$K/$1/")
	shift
	for pat in "$@"; do
		f=$(echo "$list" | grep -o "${pat}_[0-9-]*\.zim" | sort -u | tail -1)
		if [ -n "$f" ]; then echo "$f"; return; fi
	done
}

for p in $projects; do
	case $p in
	wikipedia)   folder=wikipedia; f=$(newest wikipedia wikipedia_en_all_nopic) ;;
	mathematics) folder=wikipedia; f=$(newest wikipedia wikipedia_en_mathematics_nopic wikipedia_en_mathematics_maxi) ;;
	*)           folder=$p; f=$(newest "$p" "${p}_en_all_nopic" "${p}_en_all_maxi") ;;
	esac
	if [ -z "$f" ]; then
		echo "$p: none found"
		continue
	fi
	size=$(curl -sIL "$K/$folder/$f" | grep -i '^content-length' | tail -1 | awk '{printf "%.1f GB", $2 / 1e9}')
	echo "$p: $f ($size)"
	if [ $get = 1 ]; then
		if command -v aria2c >/dev/null; then
			aria2c -c -x 8 -s 8 --file-allocation=none --follow-metalink=mem -d "$dir" "$K/$folder/$f.meta4" ||
				aria2c -c -x 8 -s 8 --file-allocation=none -d "$dir" "$K/$folder/$f"
		else
			wget -c -P "$dir" "$K/$folder/$f"
		fi
	fi
done
