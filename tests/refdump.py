#!/usr/bin/env python3
"""Reference dump of a ZIM using python-libzim, same format as `zimcat --dump`."""
import struct
import sys

from libzim.reader import Archive


def fnv1a(data):
    h = 2166136261
    for b in data:
        h ^= b
        h = (h * 16777619) & 0xFFFFFFFF
    return h


def main(path):
    a = Archive(path)
    f = open(path, "rb")
    hdr = f.read(80)
    url_ptr_pos = struct.unpack("<Q", hdr[32:40])[0]
    mime_pos = struct.unpack("<Q", hdr[56:64])[0]
    f.seek(mime_pos)
    mimes = f.read(4096).split(b"\0")
    count = struct.unpack("<I", hdr[24:28])[0]
    f.seek(url_ptr_pos)
    ptrs = struct.unpack("<%dQ" % count, f.read(8 * count))
    for i in range(count):
        f.seek(ptrs[i])
        d = f.read(16)
        mime, = struct.unpack("<H", d[:2])
        ns = chr(d[3])
        e = a._get_entry_by_id(i)
        # libzim paths are "ns/url" for old archives, "url" for new ones
        url = e.path.split("/", 1)[1] if (not a.has_new_namespace_scheme and "/" in e.path) else e.path
        if mime == 0xFFFF:
            target, = struct.unpack("<I", d[8:12])
            print("%d %s R %d - %s" % (i, ns, target, url))
            continue
        if mime >= 0xFFFD:
            print("%d %s X - - %s" % (i, ns, url))
            continue
        content = bytes(e.get_item().content)
        print("%d %s %s %d %08x %s" % (i, ns, mimes[mime].decode(), len(content), fnv1a(content), url))


if __name__ == "__main__":
    main(sys.argv[1])
