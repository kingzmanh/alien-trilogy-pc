import struct, zlib, sys
w,h=int(sys.argv[2]),int(sys.argv[3]); src=sys.argv[4] if len(sys.argv)>4 else "hr.raw"
d=open(src,"rb").read(); raw=struct.unpack('<%dH'%(len(d)//2),d)
rows=b''
for y in range(h):
    row=bytearray([0])
    for x in range(w):
        c=raw[y*w+x]; r=(c>>10)&31; g=(c>>5)&31; b=c&31
        row+=bytes([r<<3|r>>2,g<<3|g>>2,b<<3|b>>2])
    rows+=bytes(row)
def ch(t,dd): return struct.pack('>I',len(dd))+t+dd+struct.pack('>I',zlib.crc32(t+dd)&0xffffffff)
open(sys.argv[1],'wb').write(b'\x89PNG\r\n\x1a\n'+ch(b'IHDR',struct.pack('>IIBBBBB',w,h,8,2,0,0,0))+ch(b'IDAT',zlib.compress(rows))+ch(b'IEND',b''))
