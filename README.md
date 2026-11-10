# zimwiki: ZIM reader firmware for the Openmoko WikiReader

Version 1.0.41 (released as *WikiReader Gen2*). The version and build date
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
* **Fast**: about 0.4 to 0.5 s to open a typical article from a
  reclustered ZIM; a progress bar on very big pages. Scrolling is 1:1 with
  momentum.
* **Navigation like a browser**: HISTORY (short press) or the bottom left
  corner goes Back; the toolbar has *Back*, *List* (jump to a section by
  name), *Top*, *Prev/Next section*. Back remembers your scroll position.
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
# recommended: faster downloads, and the simulator window
sudo apt install aria2 libsdl2-dev pkg-config
# optional: the test suite
sudo apt install python3 python3-pip && pip install --break-system-packages libzim
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
one: full text without images, which the WikiReader cannot show anyway.
*maxi* (full text with images) works as well when it is the newer dump:
`zimprep --recluster` leaves the pictures out, so it comes out about the
size of the nopic one (the download is bigger). *mini* has only the
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

* `--recluster` rewrites the ZIM with small (128 KB) compressed clusters.
  It is still a normal ZIM (same entries, same UUID, checked with libzim),
  but the WikiReader then decompresses about 64 KB per article instead of
  about 1 MB, which is most of the time it takes to open a page. It uses
  all CPU cores. Kiwix's full text search indexes (Xapian, often several
  GB) are left out, as the WikiReader has its own title index;
  `--keep-fulltext` keeps them. So are the pictures (and redirects to
  them), which the WikiReader cannot show: the remaining entries are
  renumbered, and the ZIM's lists and the WikiReader's index with them;
  `--keep-images` keeps them. Without `--recluster` the ZIM is only
  split (minutes instead of hours, but slower page opening), pictures
  and all.
* Time: minutes for the small wikis; one to a few hours for the full
  English Wikipedia (it prints the time of each stage).
* Other options: `--chunk-mb N`, `--level N` (zstd level, default 19),
  `--threads N`, `--index-only` (rebuild just the index).

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
  **Copy them to the card too**: zimprep makes the first two (64 KB of
  zeros) and `card/ZWSTATE.DAT` is the third. The WikiReader can only
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
| HISTORY button, short press | **Back**, like a browser: the results or list you came from, or the article you followed a link from |
| HISTORY button, hold ½ s | list of recently read articles |
| RANDOM button | random article (on release; in the calculator: back to where you were) |
| RANDOM button, hold ½ s | **calculator** (hold again, or press RANDOM, SEARCH or HISTORY, to leave). Tabs at the top: Simple, Sci |
| Tap the bottom left corner of an article | Back (as on the original WikiReader) |
| Type | results update as you type; tap one to open it. Drag the results to put the keyboard away, tap the search bar to bring it back; 123 for digits and punctuation |
| Drag | scroll an article or a list (flick for momentum) |
| Tap a link | open it (dotted underline) |
| Tap elsewhere in an article | toolbar: Back, List (sections: tap one to jump, Back returns), Top, Prev/Next section |
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
* 1.0.41: zimprep --recluster leaves pictures out by default (and redirects to them), so a "maxi" ZIM gives about the same card files as a "nopic" one; --keep-images keeps them. The entries are renumbered, with the ZIM's title lists (X/listing/titleOrdered), redirects, main page and the WikiReader's index to match; metadata (the Kiwix icon) stays. Checked with libzim (integrity check, every entry by path, redirects, listings) on old and new style ZIMs, and the index gives the same article for every title as before. The test climate wiki shrinks from 8.8 to 6.5 MB. Only zimprep changed (init.app only by its version number). Source tarballs 1.0.35 to 1.0.40 held a stray Python wheel (zstandard...whl) by mistake; removed.
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
* No images, no full text search (title search only). Section links work
  when they name a heading; anchors elsewhere open the page at the top.
* Tables are simplified to one line per row; complex formulas are linear
  text (a/b fractions, sub and superscripts side by side).
* Characters outside the Basic Multilingual Plane (emoji, rare CJK, some
  ancient scripts; about 1 per 100 Wikipedia pages) show as boxes: the
  font format holds 16 bit code points. GNU Unifont's own unifont_upper.otf
  would cover them if the format were widened. Hebrew and Arabic are shown
  right to left but Arabic letters are not joined, and Indian scripts are
  not shaped.
* At most 100 pieces per wiki (200 GB with 2 GB pieces) and 32 wikis.

## Layout

    src/          the application (portable C, builds with gcc 3.3 and modern gcc)
    device/       WikiReader platform layer, Makefile, toolchain script and patch
    host/         PC platform layers (SDL, headless) and file helpers
    tools/        zimprep (+ recluster), zimcat, c33emu, getzims.sh, sim.sh,
                  mkrelease.sh, font and fold table generators
    card/         prebuilt kernel.elf, init.app, WIKIFONT.BIN; README.txt.in
    third_party/  zstd decoder (BSD), xz-embedded (public domain)
    fonts/        BDF sources and generated tables
    tests/        test suite

## Licences

zimwiki: GPL v3 (like the WikiReader software it runs on). grifo and the
WikiReader libraries: GPL v3, Openmoko. zstd: BSD. xz-embedded: public
domain. Fonts: Liberation (GPL with font exception), Junicode (OFL),
WenQuanYi (GPL with font exception), DejaVu (free licence), GNU Unifont
(GPL v2+ with font embedding exception, or SIL OFL 1.1). Wikipedia text:
CC BY-SA, shown at the end of each article.
