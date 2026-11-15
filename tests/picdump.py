#!/usr/bin/env python3
"""picdump: read a zimprep picture file (NAME.P00 ...), see src/pic.h

  picdump.py BASE                 summary: articles, pictures, sizes
  picdump.py BASE --check ZIM     every article entry is an HTML article of
                                  the prepared ZIM (BASE.Z00...), every
                                  picture's hash is the hash of an <img src>
                                  in that article; exit 1 on a problem
  picdump.py BASE --png DIR       write each article's pictures as PNG
"""
import glob, struct, sys, os

def fnv(src):
    # pic_src_hash: entities decoded, FNV-1a 32
    for e, c in (('&amp;', '&'), ('&#38;', '&'), ('&quot;', '"'), ('&#39;', "'"),
                 ('&apos;', "'"), ('&lt;', '<'), ('&gt;', '>')):
        src = src.replace(e, c)
    h = 2166136261
    for b in src.encode('utf-8', 'surrogateescape'):
        h ^= b
        h = (h * 16777619) & 0xffffffff
    return h

def load(base):
    parts = sorted(glob.glob(base + '.P[0-9][0-9]'))
    return b''.join(open(p, 'rb').read() for p in parts)

def parse(d):
    assert d[:4] == b'ZWPI', 'not a picture file'
    ver, n, shift, pieces = struct.unpack('<IIII', d[4:20])
    total = struct.unpack('<Q', d[20:28])[0]
    uuid = d[28:44]
    mode, npics, entries = struct.unpack('<III', d[44:56])
    assert total == len(d), 'size %d != %d' % (total, len(d))
    arts = []
    for i in range(n):
        e, off = struct.unpack('<IQ', d[64 + 12 * i:76 + 12 * i])
        cnt = d[off]
        pics = []
        for k in range(cnt):
            h, w, hh, bo = struct.unpack('<IHHQ', d[off + 4 + 16 * k:off + 20 + 16 * k])
            cs = bo >> 40 if ver >= 2 else 0
            if ver >= 2:
                bo &= (1 << 40) - 1
            pics.append((h, w, hh, bo, cs))
        arts.append((e, pics))
    return dict(version=ver, mode=mode, npics=npics, shift=shift, pieces=pieces, uuid=uuid, arts=arts,
                entries=entries if ver >= 2 else 0)

def bitmap(d, w, hh, bo, cs):
    '''the bitmap's bytes (version 2: zstd frames need the zstandard module)'''
    size = ((w + 7) // 8) * hh
    if not cs:
        return d[bo:bo + size]
    import zstandard
    return zstandard.ZstdDecompressor().decompress(d[bo:bo + cs], max_output_size=size)

def main():
    base = sys.argv[1]
    d = load(base)
    p = parse(d)
    arts = p['arts']
    entries = [a[0] for a in arts]
    ok = entries == sorted(entries) and len(set(entries)) == len(entries)
    shown = sum(len(a[1]) for a in arts)
    packed = sum(1 for a in arts for x in a[1] if x[4])
    print('%d articles, %d picture slots, %d bitmaps, mode %d, %d bytes, version %d, %d compressed slots'
          % (len(arts), shown, p['npics'], p['mode'], len(d), p['version'], packed))
    for e, pics in arts:
        for h, w, hh, bo, cs in pics:
            if w:
                ok = ok and 0 < w <= 228 and 0 < hh <= 200 and bo + (cs or ((w + 7) // 8) * hh) <= len(d)
                if cs:
                    try:
                        ok = ok and len(bitmap(d, w, hh, bo, cs)) == ((w + 7) // 8) * hh
                    except ImportError:
                        pass
    if '--check' in sys.argv:
        import re
        sys.path.insert(0, os.path.dirname(__file__))
        from libzim.reader import Archive
        zim = sys.argv[sys.argv.index('--check') + 1]
        a = Archive(zim)
        ok = ok and a.uuid.bytes == p['uuid'] if hasattr(a, 'uuid') else ok
        for e, pics in arts:
            x = a._get_entry_by_id(e)
            if x.is_redirect or not x.get_item().mimetype.startswith('text/html'):
                print('entry %d is not an article' % e)
                ok = False
                continue
            html = bytes(x.get_item().content).decode('utf-8', 'surrogateescape')
            srcs = set(fnv(m) for m in re.findall(r'<img[^>]*?\ssrc="([^"]*)"', html))
            srcs |= set(fnv(m) for m in re.findall(r'<img[^>]*?\sdata-src="([^"]*)"', html))
            for h, w, hh, bo, cs in pics:
                if h not in srcs:
                    print('entry %d (%s): picture hash %08x not an <img src> of it' % (e, x.path, h))
                    ok = False
    if '--png' in sys.argv:
        from PIL import Image
        out = sys.argv[sys.argv.index('--png') + 1]
        os.makedirs(out, exist_ok=True)
        for e, pics in arts:
            for k, (h, w, hh, bo, cs) in enumerate(pics):
                if not w:
                    continue
                stride = (w + 7) // 8
                raw = bytes(255 - b for b in bitmap(d, w, hh, bo, cs))   # 1 = black -> PIL '1' 0 = black
                Image.frombytes('1', (w, hh), raw, 'raw', '1', stride).save(
                    os.path.join(out, 'e%d_%d.png' % (e, k)))
    sys.exit(0 if ok else 1)

main()
