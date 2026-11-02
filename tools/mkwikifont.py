#!/usr/bin/env python3
"""Make the card's fallback font WIKIFONT.BIN: the WenQuanYi CJK glyphs of
fonts/bdf/textall.bdf, plus GNU Unifont for every other character of the
Basic Multilingual Plane they lack (Devanagari, Arabic, Hebrew, Thai,
Hangul, Georgian, Ethiopic, symbols...).

  mkwikifont.py BASE.bdf unifont.otf OUT.bdf
  bdf2c.py --bin OUT.bdf WIKIFONT.BIN

unifont.otf comes with the fonts-unifont package (sudo apt install
fonts-unifont: /usr/share/fonts/opentype/unifont/unifont.otf).  Unifont
is drawn on a 16 pixel grid, so rendering it at 16 px in monochrome gives
its bitmaps exactly.  Needs freetype-py (pip install freetype-py).
GNU Unifont: GPL v2+ with the GNU font embedding exception / SIL OFL 1.1.
"""
import re
import sys
import unicodedata

import freetype

SKIP = [(0xD800, 0xDFFF), (0xE000, 0xF8FF), (0xFFF0, 0xFFFF),   # surrogates, private use, specials
        (0xFE00, 0xFE0F), (0x180B, 0x180F), (0x034F, 0x034F)]    # variation selectors, CGJ


def invisible(cp):
    """Format and control characters (zero width spaces and joiners,
    direction marks, the byte order mark): Unifont draws them as little
    labelled boxes ("ZWNBSP"), but in text they must stay invisible."""
    return unicodedata.category(chr(cp)) in ("Cf", "Cc", "Zl", "Zp")


def main():
    base, otf, outp = sys.argv[1:4]
    text = open(base, encoding="latin-1").read()
    have = set(int(x) for x in re.findall(r"^ENCODING (\d+)$", text, re.M))
    head, body = text.split("CHARS", 1)
    body = body.split("\n", 1)[1]
    body = body[:body.rindex("ENDFONT")]
    face = freetype.Face(otf)
    face.set_pixel_sizes(0, 16)
    add = []
    for cp in range(0x80, 0x10000):
        if cp in have or any(a <= cp <= b for a, b in SKIP) or invisible(cp) or \
                not face.get_char_index(cp):
            continue
        face.load_char(cp, freetype.FT_LOAD_RENDER | freetype.FT_LOAD_TARGET_MONO)
        g = face.glyph
        bm = g.bitmap
        w, h = bm.width, bm.rows
        nb = (w + 7) // 8
        rows = [bytes(bm.buffer[r * bm.pitch: r * bm.pitch + nb]).hex().upper() for r in range(h)]
        adv = (g.advance.x + 32) >> 6
        if not w or not h:
            w = h = 0
            rows = []
        add.append("STARTCHAR U+%04X\nENCODING %d\nSWIDTH 500 0\nDWIDTH %d 0\nBBX %d %d %d %d\nBITMAP\n%sENDCHAR\n"
                   % (cp, cp, adv, w, h, g.bitmap_left, g.bitmap_top - h,
                      "".join(r + "\n" for r in rows)))
    n = len(have) + len(add)
    with open(outp, "w", encoding="latin-1") as f:
        f.write(head + "CHARS %d\n" % n + body + "".join(add) + "ENDFONT\n")
    print("%s: %d glyphs (%d from %s, %d from Unifont)" % (outp, n, len(have), base, len(add)))


main()
