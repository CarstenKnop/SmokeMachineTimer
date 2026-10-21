#!/usr/bin/env python3
"""Re-compresses the PNGs written by oled_preview.exe (they use uncompressed deflate blocks, ~1.3 MB each)
with zlib level 9 so the pictures are small enough to live in git / on GitHub.   usage: compress_png.py <dir>"""
import pathlib
import struct
import sys
import zlib


def chunks(data):
    pos = 8
    while pos < len(data):
        length, ctype = struct.unpack(">I4s", data[pos:pos + 8])
        yield ctype, data[pos + 8:pos + 8 + length]
        pos += 12 + length


def pack(ctype, body):
    return struct.pack(">I", len(body)) + ctype + body + struct.pack(">I", zlib.crc32(ctype + body) & 0xFFFFFFFF)


def recompress(path):
    data = path.read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        return 0
    out, idat = [b"\x89PNG\r\n\x1a\n"], []
    for ctype, body in chunks(data):
        if ctype == b"IDAT":
            idat.append(body)
        elif ctype != b"IEND":
            out.append(pack(ctype, body))
    out.append(pack(b"IDAT", zlib.compress(zlib.decompress(b"".join(idat)), 9)))
    out.append(pack(b"IEND", b""))
    new = b"".join(out)
    path.write_bytes(new)
    return len(data) - len(new)


if __name__ == "__main__":
    folder = pathlib.Path(sys.argv[1])
    saved = sum(recompress(p) for p in folder.glob("*.png"))
    print(f"recompressed {len(list(folder.glob('*.png')))} PNGs, saved {saved // 1024} KB")
