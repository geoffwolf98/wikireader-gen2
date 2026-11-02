#!/usr/bin/env python3
"""Render glyphs of a TrueType font into a 1 bit BDF (for the maths symbols).

  ttf2bdf.py FONT.ttf PIXELS ASCENT DESCENT CODEPOINTS.txt OUT.bdf

CODEPOINTS.txt: hex code points or ranges (03b1-03c9), # comments.
Needs freetype-py (pip install freetype-py).  Uses the font's own
hinting in monochrome mode, which gives clean shapes at small sizes.
"""
import sys
import freetype


def codepoints(path):
    out = []
    for ln in open(path):
        ln = ln.split("#")[0].strip()
        for tok in ln.split():
            if "-" in tok:
                a, b = tok.split("-")
                out += range(int(a, 16), int(b, 16) + 1)
            else:
                out.append(int(tok, 16))
    return sorted(set(out))


def main():
    ttf, px, asc, desc, cps, outp = sys.argv[1:7]
    px, asc, desc = int(px), int(asc), int(desc)
    face = freetype.Face(ttf)
    face.set_pixel_sizes(0, px)
    chars = []
    for cp in codepoints(cps):
        if not face.get_char_index(cp):
            continue
        face.load_char(cp, freetype.FT_LOAD_RENDER | freetype.FT_LOAD_TARGET_MONO)
        g = face.glyph
        bm = g.bitmap
        w, h = bm.width, bm.rows
        rows = []
        nb = (w + 7) // 8
        for r in range(h):
            line = bytes(bm.buffer[r * bm.pitch: r * bm.pitch + nb])
            rows.append(line.hex().upper())
        adv = (g.advance.x + 32) >> 6
        chars.append((cp, adv, w, h, g.bitmap_left, g.bitmap_top - h, rows))
    with open(outp, "w") as f:
        f.write("STARTFONT 2.1\nFONT -zimwiki-%s-%d\nSIZE %d 75 75\n" % (face.family_name.decode(), px, px))
        f.write("FONTBOUNDINGBOX %d %d 0 %d\n" % (px * 2, asc + desc, -desc))
        f.write("STARTPROPERTIES 2\nFONT_ASCENT %d\nFONT_DESCENT %d\nENDPROPERTIES\n" % (asc, desc))
        f.write("CHARS %d\n" % len(chars))
        for cp, adv, w, h, xo, yo, rows in chars:
            f.write("STARTCHAR U+%04X\nENCODING %d\nSWIDTH 500 0\nDWIDTH %d 0\nBBX %d %d %d %d\nBITMAP\n"
                    % (cp, cp, adv, w, h, xo, yo))
            for r in rows:
                f.write(r + "\n")
            f.write("ENDCHAR\n")
        f.write("ENDFONT\n")
    print("%s: %d glyphs" % (outp, len(chars)))


main()
