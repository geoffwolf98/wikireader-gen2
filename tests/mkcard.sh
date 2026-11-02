#!/bin/sh
# mkcard.sh CARD.img SIZE_MB FILE...
# Make a FAT32 SD card image (MBR partition, like a real card) holding
# FILE... in its root folder.  Needs dosfstools, mtools and sfdisk.
set -e
img=$1
size=$2
shift 2
rm -f "$img"
truncate -s "${size}M" "$img"
printf 'label: dos\nstart=2048, type=c\n' | sfdisk -q "$img"
mkfs.vfat -F 32 --offset 2048 "$img" >/dev/null
export MTOOLS_SKIP_CHECK=1
for f in "$@"; do
	mcopy -i "$img@@1M" "$f" "::$(basename "$f")"
done
