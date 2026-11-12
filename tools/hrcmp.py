"""hrcmp.py: compare hr.raw (N x frame) and lo1.raw (the same ordering table
drawn at 1x), both 15-bit, written by an AT_HRCMP run.
Writes shots/hrcmp.png: 1x | N x (scaled to 2x) | difference (bright = the
N x frame's block average differs from the 1x pixel)."""
import array, math, os
from PIL import Image

lo = array.array('H', open('lo1.raw', 'rb').read())
hr = array.array('H', open('hr.raw', 'rb').read())
n = int(round(math.sqrt(len(hr) / (320 * 240))))
W, H = 320 * n, 240 * n


def rgb(c):
    return (((c >> 10) & 31) << 3, ((c >> 5) & 31) << 3, (c & 31) << 3)


li = Image.new('RGB', (320, 240))
li.putdata([rgb(c) for c in lo])
hi = Image.new('RGB', (W, H))
hi.putdata([rgb(c) for c in hr])
hs = hi.resize((320, 240), Image.BOX)
diff = Image.new('RGB', (320, 240))
dp = []
bad = 0
for y in range(240):
    for x in range(320):
        a = li.getpixel((x, y))
        b = hs.getpixel((x, y))
        d = sum(abs(a[i] - b[i]) for i in range(3))
        if d > 120:
            bad += 1
        dp.append((min(255, d * 2),) * 3)
diff.putdata(dp)
out = Image.new('RGB', (640 * 3, 480))
out.paste(li.resize((640, 480), Image.NEAREST), (0, 0))
out.paste(hi.resize((640, 480), Image.BILINEAR), (640, 0))
out.paste(diff.resize((640, 480), Image.NEAREST), (1280, 0))
os.makedirs('shots', exist_ok=True)
out.save('shots/hrcmp.png')
print('scale', n, 'pixels differing strongly:', bad)
