"""montage.py [cols] [scale]: shots/shot_*.bmp -> shots/montage.png (numbered tiles)"""
import glob, sys, os
from PIL import Image, ImageDraw
cols = int(sys.argv[1]) if len(sys.argv) > 1 else 4
sc = float(sys.argv[2]) if len(sys.argv) > 2 else 0.4
fs = sorted(glob.glob('shots/shot_*.bmp'), key=os.path.getmtime)
ims = [Image.open(f).convert('RGB') for f in fs]
w, h = int(ims[0].width * sc), int(ims[0].height * sc)
rows = (len(ims) + cols - 1) // cols
m = Image.new('RGB', (w * cols, h * rows), (40, 0, 40))
for i, (f, im) in enumerate(zip(fs, ims)):
    t = im.resize((w, h))
    ImageDraw.Draw(t).text((4, 4), os.path.basename(f)[5:9], fill=(255, 255, 0))
    m.paste(t, ((i % cols) * w, (i // cols) * h))
m.save('shots/montage.png')
print(len(ims), 'shots')
