"""montage4.py out.png W H a.raw b.raw c.raw d.raw: four 15-bit frames (W x H) at half size in a 2x2 grid"""
import struct, zlib, sys
out, W, H = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
def load(p):
    try: d = open(p, 'rb').read(); return struct.unpack('<%dH' % (len(d) // 2), d)
    except OSError: return None
fr = [load(p) for p in sys.argv[4:8]]
w, h = W // 2, H // 2
rows = b''
for Y in range(2 * h):
    row = bytearray([0])
    for X in range(2 * w):
        f = fr[(Y // h) * 2 + (X // w)] if (Y // h) * 2 + (X // w) < len(fr) else None
        c = f[((Y % h) * 2) * W + (X % w) * 2] if f else 0
        row += bytes([((c >> 10) & 31) << 3, ((c >> 5) & 31) << 3, (c & 31) << 3])
    rows += bytes(row)
def ch(t, d): return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
open(out, 'wb').write(b'\x89PNG\r\n\x1a\n' + ch(b'IHDR', struct.pack('>IIBBBBB', 2 * w, 2 * h, 8, 2, 0, 0, 0)) + ch(b'IDAT', zlib.compress(rows)) + ch(b'IEND', b''))
