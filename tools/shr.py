"""Unpack Alien Trilogy installer .SHR files: 226-byte header (name at 0x3a,
packed size at 0xc2) + PKWARE DCL "implode" stream.  usage: shr.py <file.shr> <outdir>"""
import os, sys

LITLEN = [11, 124, 8, 7, 28, 7, 188, 13, 76, 4, 10, 8, 12, 10, 12, 10, 8, 23, 8,
          9, 7, 6, 7, 8, 7, 6, 55, 8, 23, 24, 12, 11, 7, 9, 11, 12, 6, 7, 22, 5,
          7, 24, 6, 11, 9, 6, 7, 22, 7, 11, 38, 7, 9, 8, 25, 11, 8, 11, 9, 12,
          8, 12, 5, 38, 5, 38, 5, 11, 7, 5, 6, 21, 6, 10, 53, 8, 7, 24, 10, 27,
          44, 253, 253, 253, 252, 252, 252, 13, 12, 45, 12, 45, 12, 61, 12, 45,
          44, 173]
LENLEN = [2, 35, 36, 53, 38, 23]
DISTLEN = [2, 20, 53, 230, 247, 151, 248]
BASE = [3, 2, 4, 5, 6, 7, 8, 9, 10, 12, 16, 24, 40, 72, 136, 264]
EXTRA = [0, 0, 0, 0, 0, 0, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8]


def construct(rep):
    lens = []
    for r in rep:
        lens += [r & 15] * ((r >> 4) + 1)
    count = [0] * 14
    for l in lens:
        count[l] += 1
    offs = [0] * 14
    for l in range(1, 13):
        offs[l + 1] = offs[l] + count[l]
    sym = [0] * len(lens)
    for s, l in enumerate(lens):
        sym[offs[l]] = s
        offs[l] += 1
    return count, sym


class Bits:
    def __init__(self, data):
        self.d, self.p, self.buf, self.cnt = data, 0, 0, 0

    def bits(self, n):
        while self.cnt < n:
            self.buf |= self.d[self.p] << self.cnt
            self.p += 1
            self.cnt += 8
        v = self.buf & ((1 << n) - 1)
        self.buf >>= n
        self.cnt -= n
        return v

    def decode(self, h):
        count, sym = h
        code = first = index = 0
        for l in range(1, 14):
            code |= self.bits(1) ^ 1
            c = count[l]
            if code < first + c:
                return sym[index + (code - first)]
            index += c
            first = (first + c) << 1
            code <<= 1
        raise ValueError('bad code')


LIT, LEN, DIST = construct(LITLEN), construct(LENLEN), construct(DISTLEN)


def blast(data):
    s = Bits(data)
    lit, dict_ = s.bits(8), s.bits(8)
    assert lit in (0, 1) and 4 <= dict_ <= 6
    out = bytearray()
    while True:
        if s.bits(1):
            sym = s.decode(LEN)
            ln = BASE[sym] + s.bits(EXTRA[sym])
            if ln == 519:
                break
            sym = 2 if ln == 2 else dict_
            dist = (s.decode(DIST) << sym) + s.bits(sym) + 1
            for _ in range(ln):
                out.append(out[-dist])
        else:
            out.append(s.decode(LIT) if lit else s.bits(8))
    return bytes(out)


if __name__ == '__main__':
    raw = open(sys.argv[1], 'rb').read()
    name = raw[0x3a:raw.index(b'\0', 0x3a)].decode()
    size = int.from_bytes(raw[0xc2:0xc6], 'little')
    data = blast(raw[0xe2:0xe2 + size])
    os.makedirs(sys.argv[2], exist_ok=True)
    open(os.path.join(sys.argv[2], name), 'wb').write(data)
    print(name, len(data))
