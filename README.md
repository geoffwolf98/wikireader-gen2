# zimwiki: ZIM reader firmware for the Openmoko WikiReader

Version 1.0.53 (released as *WikiReader Gen2*). The version and build date
are shown at the bottom of the search screen; `zimprep --version` prints
it for the PC tool.

New software for the 2009 Openmoko WikiReader that reads standard Kiwix
**ZIM** files, so you can carry any current Wikipedia (or Wiktionary,
Wikivoyage, Wikibooks...) instead of the 2011 dumps the original firmware
needed.

It runs on the WikiReader's own kernel (grifo) from the microSD card.
Nothing inside the device is changed, and your original card keeps working.

Contents:

1. [What's better than the original](#whats-better-than-the-original)
2. [Quick start: a card from a release zip](#quick-start-a-card-from-a-release-zip)
3. [Setting up a PC](#setting-up-a-pc)
4. [Getting ZIM files](#getting-zim-files)
5. [Preparing a wiki with zimprep](#preparing-a-wiki-with-zimprep)
6. [Making a card](#making-a-card)
7. [Using it](#using-it)
8. [The simulator](#the-simulator)
9. [Making a release](#making-a-release)
10. [Rebuilding the fallback font](#rebuilding-the-fallback-font)
11. [Building the WikiReader binaries from scratch](#building-the-wikireader-binaries-from-scratch)
12. [Testing](#testing)
13. [Changes](#changes)
14. [Limits](#limits), [Layout](#layout), [Licences](#licences)

## What's better than the original

* **Any Kiwix ZIM**, old or new format (ZIM 5 and 6, zstd or xz).
* **Search that forgives**: case, accents and punctuation are ignored
  (`eclair` finds *Éclair*, `rock n roll` finds *Rock 'n' Roll*), redirects
  show where they lead, results update on every key. Long titles keep
  their end visible (*The Complete …/Volume 2/Chapter 1*).
* **Fast**: about 0.4 to 0.5 s to open an article of a *mini* wiki
  (introductions only; measured in the emulator). Full length articles
  (*maxi* and *nopic* wikis) take seconds, most for reading the long
  HTML, which zimprep slims to a third or so (1.0.47); a progress bar on
  big pages; tap the screen (or press a button) while it opens to stop
  and stay where you were. Scrolling is 1:1 with
  momentum.
* **Navigation like a browser**: HISTORY (short press) or the bottom left
  corner goes Back; the toolbar has *Back*, *List* (jump to a section by
  name), *Top*, *Prev/Next section* and *Find* (find in the page). Back
  remembers your scroll position.
* **History** that survives power off, and the device reopens where you
  left off.
* **Maths** shown as readable notation (∑, √, sub and superscripts,
  fractions as a/b, display matrices as real rows with brackets).
* Infoboxes as compact `Label: value` lines, lists, tables, headings.
  Navboxes, citation markers and reference lists are dropped.
* Up to 32 wikis on one card, with a picker.
* Latin, Greek, Cyrillic, IPA and maths symbols built in; optional
  `WIKIFONT.BIN` (about 2 MB, read only when a page needs it) adds about
  57,000 glyphs: Chinese and Japanese, and from GNU Unifont Devanagari and
  the other Indian scripts, Hebrew, Arabic, Thai, Korean, Georgian,
  Armenian, Ethiopic and symbols. Hebrew and Arabic are shown right to
  left; Arabic letters are not joined, and Indian scripts are not shaped
  (letters right, some marks in a simple position).

## Quick start: a card from a release zip

1. Format a microSD card as **FAT32** (one partition, MBR). Cards over
   32 GB are sold as exFAT and must be reformatted.
2. Extract `WikireaderGen2.VERSION.zip` to the top of the card.
3. Put it in the WikiReader and switch on. Each wiki shows *Preparing* with its name
   for a few seconds the first time only.

The rest of this file is for making your own cards, adding wikis and
building everything from source.

## Setting up a PC

Linux (Debian, Ubuntu, Mint; other distributions have the same packages
under similar names). macOS should work for the PC tools with Homebrew.

```sh
# required: the PC tools (zimprep) and card tools
sudo apt install build-essential libzstd-dev zlib1g-dev dosfstools mtools fdisk curl zip
# recommended: pictures (zimprep --pictures; without it zimprep builds without them)
sudo apt install libwebp-dev pkg-config
# recommended: faster downloads, and the simulator window
sudo apt install aria2 libsdl2-dev pkg-config
# optional: the test suite
sudo apt install python3 python3-pip python3-pil && pip install --break-system-packages 'libzim<3.8'
# optional: checking new SD cards are genuine
sudo apt install f3
# only for building the WikiReader binaries yourself (prebuilt ones are in card/)
sudo apt install gcc-multilib bison flex patch git wget guile-3.0
```

Then build:

```sh
tar -xmzf zimwiki-VERSION.tar.gz      # or: git clone ... zimwiki
cd zimwiki
make
build/host/zimprep --version
```

`make` builds `zimprep` and `zimcat` (PC tools), the simulators
(`zimwiki-sdl` when SDL is installed, `zimwiki-headless`), the C33
emulator and the test programs, all under `build/`.

When unpacking a newer tarball over an older tree, use `tar -xmzf` (the `m`
gives files the current time), otherwise `make` may not notice changed
files. `rm -rf build && make` always rebuilds everything.

## Getting ZIM files

ZIMs come from <https://library.kiwix.org> (browse) or
<https://download.kiwix.org/zim/> (direct). Choose **nopic** where there is
one: full text without images. *maxi* (full text with images) works as
well when it is the newer dump: `zimprep --recluster` leaves the pictures
out, so it comes out about the size of the nopic one (the download is
bigger). A maxi ZIM is also what you need for pictures on the WikiReader
(`--pictures`, below). *mini* has only the
introduction of each article.

`tools/getzims.sh` finds the newest English ZIM of each project and its
size, and downloads them (with aria2c when installed: several connections
and mirrors, resumable):

```sh
mkdir -p ~/zims
tools/getzims.sh                               # list names and sizes
tools/getzims.sh --get ~/zims mathematics      # download one
tools/getzims.sh --get ~/zims                  # download all of them
```

Projects: `wikipedia` (full English, about 50 GB), `mathematics` (about
1 GB), `wiktionary`, `wikivoyage`, `wikibooks`, `wikiquote`, `wikisource`,
`wikiversity`. Late 2026 sizes were about 9, 0.3, 3.5, 0.4, 3.9 and 1.6 GB
for the sister projects.

## Preparing a wiki with zimprep

Every wiki gets a short name (1 to 8 letters or digits) and a folder of the
same name:

```sh
build/host/zimprep -n MATHS --recluster ~/zims/wikipedia_en_mathematics_nopic_2026-09.zim ~/card/MATHS
```

This writes `MATHS.IDX` (the search index) and `MATHS.Z00`, `MATHS.Z01`...
(the ZIM in 2 GB pieces, because FAT32 cannot hold files over 4 GB).

* `--recluster` rewrites the ZIM with small (64 KB; 128 KB with
  `--no-slim`) compressed clusters.
  It is still a normal ZIM (same entries, same UUID, checked with libzim),
  but the WikiReader then decompresses a few tens of KB per article
  instead of about 1 MB, which is most of the time it takes to open a
  page. It uses
  all CPU cores. Kiwix's full text search indexes (Xapian, often several
  GB) are left out, as the WikiReader has its own title index;
  `--keep-fulltext` keeps them. So are the pictures (and redirects to
  them), which the WikiReader cannot show: the remaining entries are
  renumbered, and the ZIM's lists and the WikiReader's index with them.
  Each article's HTML is also **slimmed** to what the WikiReader reads
  (its tags and the few attributes and class names it uses; no styles,
  scripts, MathML or hidden boxes): about a third of the size, so the
  wiki is smaller and articles open quicker (emulated: a 494 KB article's
  first screen in 0.63 s instead of 2.64 s; short articles 16% quicker,
  and the climate test wiki 23% smaller). Every article shows exactly
  as before (`build/host/slimcheck --pair INPUT.zim PREPARED.zim`, with
  the `.Z` pieces joined, checks a whole wiki). Kiwix on a PC would show
  the pages plain; `--no-slim` keeps the HTML as it is.
  Without `--recluster` the ZIM is only
  split (minutes instead of hours, but slower page opening), pictures
  and all.
* Time: minutes for the small wikis; a few hours for the full English
  Wikipedia (it prints the time of each stage, and how long it waited on
  reading and on compressing). Reading, slimming and compressing run at
  the same time on all cores, and so does the picture search.
* Figures, English Wikipedia maxi 2026-08 (127.4 GB, 30.1 million
  entries, 32 threads): the text step (`--recluster`, 1.0.47) took 39 min
  and made 29.6 GB of ZIM pieces (HTML slimmed from 531.6 to 145.6 GB);
  `--pictures all` made 19.5 GB with 1.0.46 (7.15 million pictures in
  4.66 million articles; Atkinson dithering makes about 8% less). So
  about 48 GB in all: a 64 GB card. Without slimming (1.0.46) the ZIM
  pieces were 74.0 GB and the text step 1 h 50 min.
* `--pictures lead` or `--pictures all` (needs a *maxi* ZIM, and zimprep
  built with libwebp-dev installed) also writes `NAME.P00`, `NAME.P01`...:
  the article pictures turned into black and white bitmaps (Atkinson
  dithering, at most 228 x 200 pixels; `--dither fs` for the softer
  Floyd-Steinberg of 1.0.43 to 1.0.47, which keeps pale shading but
  leaves more stray dots) that the WikiReader shows in the text where they
  were. `lead` keeps each article's first picture (usually the infobox
  one), `all` up to 32 per article. Icons, maths formulas (shown as text
  already), SVG drawings and pictures under 24 pixels are left out; a
  picture used in many articles is stored once, each one compressed
  (about a quarter smaller). A picture takes about 2.7 KB (at most
  5.7 KB): `all` made 19.5 GB for the 2026 English Wikipedia (see
  below), `lead` is a fraction of that.
  Conversion uses all CPU cores and adds to the time. The default is
  `none`. Never add `--keep-images` (an old option, no longer listed): it
  keeps the original pictures inside the `.Z` files too, which the
  WikiReader cannot show, and zimprep refuses it with `--pictures`.
* `--add-pictures lead` or `--add-pictures all` adds the pictures to a
  wiki you already made, without reclustering again:
  `zimprep -n NAME --add-pictures lead INPUT.zim OUTDIR`, with the same
  ZIM file and the folder holding that wiki's `NAME.Z00`... zimprep checks
  that the wiki was made from that ZIM (it refuses another ZIM, or a
  build whose entry numbers differ), then writes only `NAME.P00`...: copy
  those to the card next to the wiki's other files. It still reads every
  article and converts the pictures, but skips the hours of compressing.
  Running it again with the other mode replaces the picture files.
* Other options: `--chunk-mb N`, `--level N` (zstd level, default 19:
  higher levels gain nothing with small clusters), `--threads N`,
  `--index-only` (rebuild just the index), `--no-slim`.
* Two stages work too: `--recluster` first (a complete wiki without
  pictures), `--add-pictures lead|all` any time later with the same ZIM
  file.

Suggested names:

| ZIM | Name |
|---|---|
| Wikipedia (all) | `ENPEDIA` |
| Wikipedia mathematics | `MATHS` |
| Wiktionary | `WIKTION` |
| Wikivoyage | `WIKIVOY` |
| Wikibooks | `WIKIBOOK` |
| Wikiquote | `WIKIQUOT` |
| Wikisource | `WIKISRC` |
| Wikiversity | `WIKIVERS` |

**Always prepare wikis with zimprep.** It makes the search index, the
reclustered ZIM and the two files the WikiReader saves into (`NAME.HIS`,
`NAME.EXT`), so the device never has to create a file. A plain `NAME.ZIM`
copied to the card does still open (with a simple, case-sensitive title
search and slow pages), but the device then has to create those files
itself, which on a nearly full card can freeze or reset it. It is not
supported.

## Making a card

A card holds the software and one folder per wiki:

```
kernel.elf          grifo, the WikiReader kernel      (card/)
init.app            zimwiki                           (card/)
WIKIFONT.BIN        optional CJK and extra glyphs     (card/)
ZWSTATE.DAT         empty, filled in by the app       (card/)
MATHS/MATHS.IDX, MATHS.Z00 ..., MATHS.HIS, MATHS.EXT  (from zimprep)
ENPEDIA/...
```

The easiest way is a staging folder holding exactly that, copied to the
card. With the files from `card/` (or `device/build/card/` if you built
them yourself):

```sh
cp card/kernel.elf card/init.app card/WIKIFONT.BIN card/ZWSTATE.DAT ~/card/
```

Format and copy (check the device name with `lsblk` first; everything on
the card is erased):

```sh
echo 'start=8192, type=c' | sudo sfdisk /dev/sdX
sudo mkfs.vfat -F 32 -s 128 -n WIKIREADER /dev/sdX1   # 64 KB clusters
sudo mount /dev/sdX1 /mnt
sudo cp -r ~/card/* /mnt/
sync && sudo umount /mnt
```

Notes:

* **Card size**: FAT32 and the WikiReader's SD code work up to 2 TB in
  principle; tested with 8 GB and 64 GB cards. A blank FAT32 card with
  only these files boots and runs (nothing from the original firmware is
  needed). The full English Wikipedia
  needs a 64 GB card, Wikipedia plus all the sister projects about 75 GB
  (a 128 GB card). Some cards are not recognised at all: the WikiReader's
  boot loader (in its own flash) starts cards at 12 MHz instead of the
  400 kHz the SD standard asks for, and a few newer or budget cards do
  not answer. The WikiReader then shows its contrast menu, as with no card
  (seen with a Gigastone 128 GB A1 card). Known to work: SanDisk Extreme
  PRO 64 GB (A2) and SanDisk Ultra PLUS 200 GB (U1). Try another make. Large cheap
  cards are often fake: check them with
  `f3write /mnt && f3read /mnt` before use.
* **Folders**: wikis go in the top folder or one folder down, not deeper.
  Each wiki's history and file map (`NAME.HIS`, `NAME.EXT`) live next to
  its files, and `ZWSTATE.DAT` (last wiki and position) in the top folder.
  **Copy them to the card too**: zimprep makes the first two (64 KB and,
  from 1.0.47, 256 KB of zeros: room for the map of a wiki copied onto a
  fragmented card) and `card/ZWSTATE.DAT` is the third. The WikiReader can only
  create files slowly on a nearly full card (its FAT code searches the
  whole card for free space, long enough to freeze or reset the unit), so
  it should only ever overwrite them. For a wiki prepared with an older
  zimprep: `head -c 65536 /dev/zero > NAME.HIS` (and the same for `.EXT`).
* **Adding a wiki later**: prepare it with zimprep and copy its folder to
  the card. There is nothing to edit; the app finds wikis by itself at
  start-up.
* **Upgrading the software**: copy the new `init.app` over the old one.
* **The original menu**: on a card made from the original WikiReader image,
  the original `init.app` is a menu (with Forth diagnostics) that starts
  `wiki.app` via `init.ini`. You can keep that and install zimwiki as
  `wiki.app` instead, or replace `init.app` with zimwiki (faster start,
  keep the old one as `init.app.orig`). A blank card only needs zimwiki as
  `init.app`.

## Using it

| Control | Action |
|---|---|
| SEARCH button | search screen, as you left it; again to clear the query |
| Tap the wiki's name in the empty search bar | choose another wiki (when the card has more than one) |
| Footnotes / Citations on the empty search screen | switch [a] notes and [1] citations on or off (from the next article; remembered). With them on, tap a marker to jump to its note, ^ to jump back |
| Large text on the empty search screen | switch the article text between normal (Liberation 15 px) and large (18 px; titles, headings and small print grow too). Applies at once to the open page, keeping your place; remembered |
| Pictures on the empty search screen (wikis made with `--pictures`) | switch the pictures on or off. Applies at once to the open page, keeping your place; remembered |
| Tap the screen or press a button while a page is opening | **stop** opening it: you stay where you were (the page you were reading, the results, the list); nothing is added to history. When it is the page reopened at start, you get an empty search and the next start does not try it again. A WikiReader font load (WIKIFONT.BIN, for Chinese, Japanese and other scripts) is never cut short: the tap takes effect as soon as it has loaded |
| SEARCH held from the WikiReader logo until the message (held earlier, the boot loader opens its contrast menu) | **safe mode**: forgets the page it would reopen and the settings, and makes the wiki's file map again; history is kept. For when it crashes at start |
| HISTORY button, short press | **Back**, like a browser: the results or list you came from, or the article you followed a link from |
| HISTORY button, hold ½ s | list of recently read articles |
| RANDOM button | random article (on release; in the calculator: back to where you were) |
| RANDOM button, hold ½ s | **calculator** (hold again, or press RANDOM, SEARCH or HISTORY, to leave). Tabs at the top: Simple, Sci |
| Tap the bottom left corner of an article | Back (as on the original WikiReader) |
| Type | results update as you type; tap one to open it. Drag the results to put the keyboard away, tap the search bar to bring it back; 123 for digits and punctuation |
| Drag | scroll an article or a list (flick for momentum) |
| Tap a link | open it (dotted underline) |
| Tap elsewhere in an article | toolbar: Back, List (sections: tap one to jump, Back returns), Top, Prev/Next section, Find |
| Find (toolbar) | find in the page: type and it searches as you go (case, accents and punctuation ignored, parts of words too); the match's first letter is shown inverted with a mark in the margin. Drag or tap the page to put the keyboard away; the bar shows ◀ 3/17 ▶ ✕; tap the word on the bar to type again. Tap the page again, ✕ or any button to stop. Find again in the same page offers the last word. Captions hidden with Pictures off are not searched |
| Power | saves your place and switches off (also after 2 minutes idle) |

## The simulator

The same application built for the PC, in a window, reading a card image:

```sh
tools/sim.sh ~/card              # every wiki in the staging folder
tools/sim.sh ~/card MATHS        # just one (quick to build)
SCALE=2 tools/sim.sh ~/card      # smaller window (default 3x)
```

Mouse = finger; F1 = SEARCH, F2 = HISTORY (tap: back, hold: list),
F3 = RANDOM; typing goes to the search box; Backspace = back; Esc quits.

The image (`build/sim.img`) is built once from the staging folder and
reused until the list of wikis changes. It is a sparse file, but building
it copies the wikis, so with the full Wikipedia it takes a few minutes and
needs that much free disk. The simulator's history and state are kept in
`build/sim-state`. `build/host/zimwiki-sdl CARD.img STATEDIR SCALE` runs it
on any card image, including a real card (`/dev/sdX`, read only).

If the window fails to open from a conda shell, run `conda deactivate`
first (conda's libraries can clash with the system SDL).

## Making a release

```sh
tools/mkrelease.sh ~/card                          # WikireaderGen2.VERSION.zip
FEEDBACK="reply in the r/... thread" tools/mkrelease.sh ~/card
tools/mkrelease.sh --no-source ~/card              # binaries only, "source soon"
```

The zip holds `kernel.elf`, `init.app`, `WIKIFONT.BIN`, a `README.txt` for
end users (from `card/README.txt.in`), every wiki folder in the staging
folder and, unless `--no-source`, `source/zimwiki-VERSION.tar.gz`. People
extract it to the top of a blank FAT32 card.

The software on its own (no wikis), for new cards and for updating old
ones, as a folder, optionally uploaded to an archive.org item:

```sh
tools/mkroot.sh ~/release/WikireaderGen2_Root
tools/mkroot.sh ~/release/WikireaderGen2_Root WikireaderGen2_Root
```

The upload puts the folder in the item under its own name (like the main
release item), and removes older source tarballs from it. `FEEDBACK=`
works here too.

Before a release:

1. Put the new number in `src/zw.h` (`ZW_VERSION`), add a line to
   [Changes](#changes) and update the version at the top of this file.
   Every update that goes to anyone gets the next number (1.0.9, 1.0.10...).
2. `make && make -C device WR=~/wikireader card` (or use the prebuilt
   `card/` files if the device code did not change), then
   `cp device/build/card/init.app card/`.
3. Run the tests (below), and boot a freshly made card on a real unit.

## Rebuilding the fallback font

`WIKIFONT.BIN` is prebuilt in the source. To make it again (for example
from a newer Unifont):

```sh
sudo apt install fonts-unifont
pip install freetype-py      # --break-system-packages on some systems
make wikifont                # or: make wikifont UNIFONT=/path/to/unifont.otf
```

This merges `fonts/bdf/textall.bdf` (WenQuanYi) with every other Basic
Multilingual Plane character Unifont has, writes `WIKIFONT.BIN` and copies
it to `card/`.

## Building the WikiReader binaries from scratch

Prebuilt `kernel.elf` and `init.app` are in `card/`; you only need this to
change the device software. It needs the extra packages listed under
[Setting up a PC](#setting-up-a-pc).

1. Get the original WikiReader source:

       git clone https://github.com/wikireader/wikireader ~/wikireader

2. Build the C33 cross compiler (binutils 2.10.1 and gcc 3.3.2 with the
   WikiReader patches, about a minute):

       sh device/build-toolchain.sh ~/wikireader

   **The compiler is built as a 32-bit program on purpose.** Built as a
   normal 64-bit program, gcc 3.3 silently drops the top half of 64-bit
   constants, which breaks zstd (and anything else using 64-bit maths)
   with no warning. The script checks for this. It also patches gcc 3.3
   for modern bison and glibc. If a tarball download is blocked, put the
   file in `~/wikireader/host-tools/download/` by hand.

3. Build grifo (the kernel) and its libraries, with the toolchain on PATH:

       export PATH=~/wikireader/host-tools/toolchain-install/bin:$PATH
       cd ~/wikireader/samo-lib/include && cp config.h-default config.h
       # edit config.h: uncomment  #define BOARD_SAMO_V1 1
       for d in mini-libc fatfs drivers grifo; do make -C ~/wikireader/samo-lib/$d; done

   grifo's Makefile ends with a Qt simulator step that fails without qmake;
   `grifo.elf` is already built by then.

4. Build zimwiki for the device:

       make -C device WR=~/wikireader card

   This puts `kernel.elf` (grifo), `init.app` (zimwiki) and `WIKIFONT.BIN`
   in `device/build/card/`, and `device/build/init.elf` (with symbols) for
   the emulator.

Device code rules learnt the hard way: no unaligned 32-bit memory access
(the CPU faults); no `snprintf` (the mini libc one is not dependable);
anything that runs for seconds must call `ZW_PROGRESS()` or `ZW_KEEPALIVE()`
now and then, or grifo's watchdog resets the unit after 20 s.

## Testing

* `make test ZIMS="a.zim b.zim"`, or
  `ZIM_TESTS=path/to/zim-testing-suite/data make test` with a checkout of
  <https://github.com/openzim/zim-testing-suite>: the ZIM reader checked
  entry by entry against libzim, the index, recluster validity, FAT32, and
  the app on both the PC build and the emulated WikiReader build (including
  a power-off and restart check).
* `build/c33emu card/init.elf CARD.img SCRIPT [STATEDIR]` (or `device/build/init.elf`; it needs the binary with symbols, not `init.app`) runs the
  **actual WikiReader binary** in an Epson C33 instruction emulator, with
  grifo's system calls answered from the card image. It stops on any
  illegal or misaligned memory access, fails a run that goes 5 s without
  feeding the watchdog, and its instruction counts give rough device
  timings. `-p` adds a profile. `C33_FRAMES=prefix` saves a PNG whenever the
  screen changes during long work.
* `build/host/zimwiki-headless CARD.img SCRIPT [STATEDIR]`: the PC build,
  scripted. Script commands for both: `type TEXT`, `tap X Y`, `press X Y MS`,
  (headless also `down X Y` and `up X Y`),
  `drag X1 Y1 X2 Y2 MS`, `button NAME`, `hold NAME MS`, `bounce NAME`,
  `wait MS`, `shot FILE.png`, `expect TEXT`, `describe` (headless also
  `restart`, `stats`).
* `tests/mkcard.sh CARD.img SIZE_MB FILE...` makes a FAT32 card image.
* `tests/diff/`: the same C checks built for PC and C33, outputs compared;
  a quick way to catch compiler problems.

## Changes

* 1.0.14: taller keyboard keys (26 px rows); the pressed letter pops up large above the key.
* 1.0.15: key rows back to 23 px so 5 search results show above the keyboard; the large letter pop-up stays.
* 1.0.16: up to 32 wikis per card (was 8); zimprep warns when a ZIM is mostly PDFs, video or pictures, and its progress lines no longer overwrite each other; SanDisk Ultra PLUS 200 GB added to the known-good cards.
* 1.0.17: the wiki list shows how many wikis there are, and never scrolls past the end (it could open showing only the last few). README lists guile-3.0 for building the device software.
* 1.0.18: big compressed blocks (long books) are unpacked in one pass: the buffer is sized from the zstd frame header instead of growing and starting again, which on the WikiReader meant reading the block from the card again. The harmless "zstd error 70" lines are gone.
* 1.0.19: the "Opening…" bar gives laying out the page half its length (it had 10%, but on long books it takes most of the time, so the bar sat at 91%).
* 1.0.20: long pages open much sooner: only the first screens are laid out before the page shows, the rest follows in idle time (a 1.3 MB book: 24.8 s to 4.0 s in the emulator). Text is collected in bigger pieces, ASCII widths come from a table, and the parser and layout are built for speed (-O2). List and Next section finish the layout first ("Please wait").
* 1.0.21: the "Opening…" box shows the page size, e.g. "Opening… (1.2 MB)", and the line under the percentage says what is happening (unpacking from the card, reading the text, laying out the page).
* 1.0.22: tools/mkroot.sh makes (and can upload) the software-only release; README.txt explains updating a card without losing your place.
* 1.0.23: redirects stored as a small page that sends the browser on (meta refresh, used for redirects to a section, e.g. "Left quasi-reflexivity") now open the target article at that section instead of an empty page. tools/mkroot.sh uploads the release as a folder, like the main item.
* 1.0.24: ticks and crosses: {{Y}}/{{N}} table marks (a picture plus a hidden Y or N) show as ✓ and ✗, and ✔ ✗ ✘ ☐ ☑ ☒ were added to the maths fonts. In tables, "Total,<br>Semiconnex" no longer gets a double comma and runs of empty cells no longer leave rows of dots.
* 1.0.25: WIKIFONT.BIN adds GNU Unifont (57,407 glyphs, 1.95 MB): Devanagari, Bengali, Tamil and the other Indian scripts, Hebrew, Arabic, Thai, Korean, Georgian, Armenian, Ethiopic and more no longer show as boxes. Hebrew and Arabic runs are shown right to left. The font is read in pieces with the watchdog fed. New: make wikifont, tools/mkwikifont.py.
* 1.0.26: invisible characters (zero width no-break space, joiners, direction marks and isolates) are no longer drawn as Unifont's labelled boxes ("ZWNBSP"): they are left out of WIKIFONT.BIN (57,313 glyphs) and treated as zero width. tools/sim.sh copies WIKIFONT.BIN into the state folder on every start.
* 1.0.27: links to a section ("Page#Section") open the page at that heading, and links within the page ("#Section") scroll to it with Back returning. A section that cannot be found opens the page at the top, or leaves you where you are for a link within the page.
* 1.0.28: Back to the search results returns to where the list was scrolled (not to the top of the search again); Back to the History list keeps its place too.
* 1.0.29: SEARCH from an article also returns to the results scrolled as they were left (pressing it again still clears).
* 1.0.30: SEARCH keeps the keyboard as you left it: hidden to see more results stays hidden (it still comes up for an empty search, and after the second press that clears).
* 1.0.31: explanatory notes are shown: a Notes section (reference lists with a group, from {{efn}} and {{notelist}}) appears at the end of the article, while citation lists stay hidden. Headings left with nothing under them (a Notes section of citations only, a gallery) are dropped.
* 1.0.32: two switches on the empty search screen, kept in ZWSTATE.DAT: Footnotes (explanatory notes: [a] markers and the Notes list, lettered a. b. c.) and Citations ([1] markers and the References list). Both are off by default, which is the plain text of 1.0.30. When on, tapping a marker jumps to its note and the ^ in front of a note jumps back to the marker; Back returns each time.
* 1.0.33: keyboard in the style of the original firmware: three rows of ten 28 px keys (QWERTYUIOP / ASDFGHJKL← / ZXCV space BNM 123), light keys on a dark board with bold capital labels, each key's touch area its whole cell. Go and the hide key are gone: tap a result, and drag the results to put the keyboard away.
* 1.0.34: no code changes: example paths in the mkroot instructions made generic (~/release/...).
* 1.0.35: the Large text switch (third switch on the empty search screen, already in the code) is documented in Using it; small print colon no longer runs into the letter before it ("Large text: on" showed as "Large text on").
* 1.0.36: a spinner (| / - \ in a small box, top right) while WIKIFONT.BIN loads. It is read the first time a screen needs a character outside the built-in fonts (once per power on, a few seconds on the device), which looked like a hang, e.g. on the first search results with such a title.
* 1.0.37: calculator: hold RANDOM. Simple mode (a row of mode tabs at the top, for Scientific and Programmer later): + − × ÷, brackets (closed for you at =), %, ±, Ans, DEL, AC, with a live preview of the result while typing. Ten significant digits, exponent form from 1e10 and below 0.0001. 50+10% is 55 and 80−25% is 60, as on pocket calculators; 2(3+4) multiplies. After =, a number starts a new sum and an operator carries on from the answer. Ans is kept in ZWSTATE.DAT. RANDOM now acts on release, so it can tell a press from a hold. New: src/calc.c, tests/calctest.c (checked against printf on 200,000 numbers; results identical on the emulated WikiReader).
* 1.0.38: Sci calculator tab: sin cos tan and their inverses (2nd), ln, log, e^x and 10^x (2nd), square root, x², x³, 1/x, x^y, x!, π, e, EXP (1.5E-3), DEG/RAD. 2nd changes the outlined keys for one press. sin 30 is 0.5 and tan 90 is "Math error" in degrees; sin π is 0 in radians. The mode and the angle unit are kept in ZWSTATE.DAT. The maths functions are written in src/calc.c (the device has no maths library) and checked against the PC's on 1.1 million values (worst error 1.7e-14, far below the ten digits shown); 300 sums give identical results on the emulated WikiReader.
* 1.0.39: power off (or auto power off) with the calculator open came back on the search screen instead of the article underneath, so RANDOM then gave a random article. The screen under the calculator is now saved. New test for it (PC and emulated).
* 1.0.40: a button press could be ignored after the device had slept: grifo stops the timer behind the app's clock while the CPU sleeps, so the next press looked like contact bounce of the last release (within 80 ms on the stopped clock). Most visible as RANDOM not leaving the calculator after holding RANDOM to open it, but SEARCH and HISTORY could miss a press the same way. The app now stays awake for the 80 ms after a release. zimwiki-headless now stops its clock too whenever the app would sleep (ZW_CLOCK_RUNS=1 for the old behaviour); new test for it.
* 1.0.41: zimprep --recluster leaves pictures out by default (and redirects to them), so a "maxi" ZIM gives about the same card files as a "nopic" one; --keep-images kept them (no longer listed from 1.0.46). The entries are renumbered, with the ZIM's title lists (X/listing/titleOrdered), redirects, main page and the WikiReader's index to match; metadata (the Kiwix icon) stays. Checked with libzim (integrity check, every entry by path, redirects, listings) on old and new style ZIMs, and the index gives the same article for every title as before. The test climate wiki shrinks from 8.8 to 6.5 MB. Only zimprep changed (init.app only by its version number). Source tarballs 1.0.35 to 1.0.40 held a stray Python wheel (zstandard...whl) by mistake; removed.
* 1.0.42: zimprep stopped after reclustering a 2026 English Wikipedia maxi ZIM ("title list has 30142538 of 21673153 entries"): that ZIM has no usable title list in its header. zimprep now checks the list at the start (so a problem shows in minutes, not after hours) and, when it is missing or incomplete, rebuilds it by sorting the entries by namespace and title; on the test ZIMs the rebuilt list is byte for byte the original. Before 1.0.41 such a list was copied as it was, unchecked (only plain, unprepared ZIMs use it on the WikiReader).
* 1.0.43: Pictures. `zimprep --pictures lead|all` turns a maxi ZIM's article pictures (JPEG, PNG, WebP, GIF) into dithered black and white bitmaps in `NAME.P00`...; the WikiReader shows them in the article where they were (infobox and figure pictures, centred, with the caption below), reading each from the card as it scrolls into view (about 110 to 140 ms more to open an article with pictures, emulated). A Pictures switch on the empty search screen turns them off and on, keeping your place (also in Back and History). Picture files made from another ZIM are ignored. Tests: a test ZIM with the awkward cases (data-src, noscript copies, ../ paths, encoded names, maths and icons to ignore, a shared picture, a redirect), on the PC and emulated builds. zimprep needs libwebp-dev for this (it builds without it, and then refuses --pictures).
* 1.0.44: Picture captions: the caption under a picture (figure, thumbnail, gallery and infobox captions) is shown in small text, centred under the picture, and goes with it when Pictures is off. 1.0.43 hid them (its notes said otherwise). Device only: wikis made with 1.0.43's zimprep get them as they are.
* 1.0.45: Scrolling over pictures no longer stalls or overshoots. Reading a picture from the card held up the page while the finger was moving, and the movement made during the wait arrived as one jump that was taken as a fast flick, so the page flew on after the finger lifted. Now a picture not read yet is drawn as a frame while the finger is down or the page is gliding, and read once it stops; and a flick that spans any hold-up (more than 50 ms) is not turned into a glide. Pictures are also read further ahead (4 strips, 256 rows, below the screen) while nothing is happening. Tests: swiping a long page with a slow card (ZW_SECTOR_US) and a fling after a slow frame (new script commands move and slowdraw). Device only.
* 1.0.46: Picture files about a third smaller: each bitmap is compressed on its own (zstd) and unpacked as it is read, adding about 10 ms per picture on the device (picture file format version 2; version 1 files from 1.0.43 to 1.0.45 still work). `zimprep --add-pictures lead|all` adds pictures to a wiki already made, checking it was made from the same ZIM with the same numbering, without reclustering again. `--keep-images` is no longer listed and is refused with `--pictures`: together they stored every picture twice (the originals in the .Z files, which the WikiReader cannot show, and the bitmaps), e.g. 103 GB instead of about 59 GB for a 2022 English Wikipedia. Version 2 picture files record which build they belong to, so pictures from another build of the same ZIM (different numbering) are ignored instead of showing wrong pictures; a run without pictures deletes an older run's picture files. Both init.app and zimprep changed.
* 1.0.47: zimprep slims each article's HTML to what the WikiReader reads (63% less HTML on the climate test wiki; with clusters of 64 KiB, the new default when slimming, the ZIM pieces are 23% smaller and short articles open 16% quicker, and a 494 KB article's first screen takes 0.63 s instead of 2.64 s, emulated; `--no-slim` keeps the HTML and 128 KiB), checked by a new tool, `slimcheck`, that converts every article both ways with the device's own converter, with each setting of Footnotes, Citations and Pictures, and compares the results (no differences on the test wikis, nor on a test ZIM of awkward HTML: {{Y}}/{{N}} ticks, pre, maths, notes, picture boxes, odd markup). Reclustering is now a pipeline: reading, slimming and compressing overlap instead of taking turns, and it prints how long it waited on each. The picture search runs on all threads (the full English Wikipedia took 71 minutes on one), each reading its block of articles in cluster order; the output is the same on any number of threads. The running total time carries on through the picture stages, and the message about leaving out pictures no longer says the WikiReader shows none. zimprep makes NAME.EXT 256 KB (room for about 21,000 runs instead of 5,400), so a wiki copied onto a fragmented card does not prepare at every start; the device reads only its first 64 KB unless the map is longer. Tested with a fragmented card image.
* 1.0.48: Pictures use Atkinson dithering: cleaner maps and diagrams (white stays white), clearer shapes in photos, and 8 to 11% smaller picture files; it loses some pale shading, so `--dither fs` keeps the old Floyd-Steinberg. Re-run `--add-pictures` to convert a wiki's pictures again. zimprep makes any missing folders in OUTDIR (like mkdir -p) and checks it before reading the ZIM, instead of failing after the title list. slimcheck shows its progress. The card's README.txt covers the calculator, Large text and Pictures. Releases now come as one zip for the card (kernel.elf, init.app, WIKIFONT.BIN, ZWSTATE.DAT, README.txt and the source) plus the source tarball.
* 1.0.49: `--add-pictures` ran on one thread unless given `--threads` (the default, all CPUs, was worked out after it branched off); it now uses all CPUs and says how many. The slimming line prints GB for big wikis. Figures from the full English Wikipedia (2026-08 maxi, 32 threads, 1.0.47): reclustering 37 min (was 107), the whole text step 39 min (was 1 h 50), HTML 531.6 GB slimmed to 145.6 GB, ZIM pieces 29.6 GB (were 74.0).
* 1.0.50: Safe mode: hold SEARCH while switching on (until the message) when it crashes or freezes at every start. It clears ZWSTATE.DAT (the page it reopens, the settings, the calculator's Ans), makes the wiki's file map again for that start, and keeps the history; the SEARCH press itself is ignored. The app reads the buttons straight from port P6 at start (one button held is fine with the boot loader; several open its menu). c33emu and zimwiki-headless take ZW_HELD=search to test it; tested on both. zimprep no longer prints "finding pictures 100%" twice.
* 1.0.53: Tap to stop: a tap on the screen or a button press while a page is opening stops it, from any route (a link, a result, RANDOM, History, Back, or the page reopened at start). You stay where you were: the old article is only replaced once the new one is complete, Back keeps its step, and nothing is added to history. Stopped at start, you get an empty search and the saved state says so, so the next start does not try the page again (this covers a page that is only slow; safe mode is still there for one that crashes). "tap to stop" is shown under the Opening bar. How: the progress callback the unpacking, HTML and layout loops already call polls for input while opening and sets zw_cancel; each loop returns ZW_ECANCEL at its next report, with checks between the stages too. The stopping touch or press is used up, release included, and other input is kept for after. The WIKIFONT.BIN load is never interrupted (it reports a different stage, which does not poll); a tap during it stops the opening just after, with the font loaded and kept. Tests: "stopat N" in PC scripts and ZW_STOP_STAGE (tap during stage N), "tap2" and ZW_EARLY_TAP in c33emu; the test ZIM gains a 390 KB page and one in Chinese and Japanese.
* 1.0.52: zimprep works out the exact size of the title index right after sorting the titles and stops there, with a clear message, if it would pass the 4 GiB limit (about 145 million titles), instead of failing at the end after the slow reclustering; it prints the size against the limit. Safe mode instructions corrected: hold SEARCH from the WikiReader logo, since held earlier the boot loader opens its contrast menu.
* 1.0.51: Find in the page: Find in the article toolbar (now six tools). It searches as you type, ignoring case, accents and punctuation like the title search, and matches parts of words. The text is folded as it is read and matched with KMP, keeping only the last few text offsets, so no copy of the article is made: emulated, a search of a 494 KB test article takes well under a tenth of a second per letter (long pages wait for a 0.25 s pause in typing). Only text that is shown counts: a caption hidden with Pictures off does not. The match's line goes a third of the way down the space above the bar, its first letter inverted and a mark in the margin. Drag the page to put the keyboard away; the bar shows the search, ◀ n/m ▶ and ✕; tap the word to type again; tap the page, ✕ or any button to stop. Find opened again in the same article offers the last word (kept in memory only). fold_utf8 and find share one character fold (fold_one). Test scripts gain expectnot; find is tested on the PC and emulated builds.
* 1.0.13: saves write only the data (a few KB) over the existing file instead of rewriting all 64 KB: quicker and much less writing to the card.
* 1.0.12: the device never grows or recreates a save file (a cut-off save left an empty file that froze the next save); a failed save is not retried in a loop (that kept the unit awake); device logging no longer uses the unreliable printf.
* 1.0.11: zimprep makes each wiki's NAME.HIS and NAME.EXT in advance, and the release has an empty ZWSTATE.DAT: creating files on a nearly full card made the WikiReader freeze or go blank (its FAT code searches the whole card for free space).
* 1.0.10: the one-off preparing screen names the wiki and shows "file N of M".
* 1.0.9: display matrices drawn as rows with lined-up columns and full height brackets (simple ones; others stay [a, b; c, d]).
* 1.0.8: long titles in results and history keep their end ("The Complete …/Volume 2/Chapter 1"), so similar titles can be told apart.
* 1.0.7: WIKIFONT.BIN loaded only when a page first needs it (faster start-up).
* 1.0.6: progress bar while opening big articles.
* 1.0.5: watchdog fed during long work (big pages could reset the unit); faster HTML reading; MathML no longer leaks into text; two formulas in a row both shown.
* 1.0.4: switch wiki by tapping its name on the search screen.
* 1.0.3: search cursor 1 px after the last letter.
* 1.0.2: "No wiki found" text mentions folders.
* 1.0.1: visible gap after a typed space in the search bar.
* 1.0.0: first release tested on real hardware.

## Limits

* Timings are emulator estimates (roughly 2 to 3 cycles per instruction and
  about 1 MB/s from the card) and real use on one unit.
* Pictures only with `--pictures` (1-bit, dithered, no zooming); no full text search (title search only). Section links work
  when they name a heading; anchors elsewhere open the page at the top.
* Tables are simplified to one line per row; complex formulas are linear
  text (a/b fractions, sub and superscripts side by side).
* Characters outside the Basic Multilingual Plane (emoji, rare CJK, some
  ancient scripts; about 1 per 100 Wikipedia pages) show as boxes: the
  font format holds 16 bit code points. GNU Unifont's own unifont_upper.otf
  would cover them if the format were widened. Hebrew and Arabic are shown
  right to left but Arabic letters are not joined, and Indian scripts are
  not shaped.
* At most 32 wikis per card, and 100 pieces per wiki (200 GB with 2 GB
  pieces), counting the picture pieces (`NAME.P00`...) with the ZIM's.

## Layout

    src/          the application (portable C, builds with gcc 3.3 and modern gcc)
    device/       WikiReader platform layer, Makefile, toolchain script and patch
    host/         PC platform layers (SDL, headless) and file helpers
    tools/        zimprep (+ recluster), zimcat, c33emu, getzims.sh, sim.sh,
                  mkrelease.sh, font and fold table generators
    card/         prebuilt kernel.elf, init.app, WIKIFONT.BIN; README.txt.in
    third_party/  zstd decoder (BSD), xz-embedded (public domain), stb_image (zimprep only; MIT or public domain)
    fonts/        BDF sources and generated tables
    tests/        test suite

## Licences

zimwiki: GPL v3 (like the WikiReader software it runs on). grifo and the
WikiReader libraries: GPL v3, Openmoko. zstd: BSD. xz-embedded: public
domain. stb_image: MIT or public domain. Fonts: Liberation (GPL with font exception), Junicode (OFL),
WenQuanYi (GPL with font exception), DejaVu (free licence), GNU Unifont
(GPL v2+ with font embedding exception, or SIL OFL 1.1). Wikipedia text:
CC BY-SA, shown at the end of each article.
