"""Split a CloneCD .ccd/.img (raw 2352-byte sectors) into a 2048-byte ISO of
track 1 and trackNN.wav files for the audio tracks + toc.txt (cdaudio.c format).
usage: ccd_split.py <file.ccd> <out.iso> <cdaudio dir>"""
import os, re, struct, sys

ccd, out_iso, out_dir = sys.argv[1:4]
img = os.path.splitext(ccd)[0] + '.img'
text = open(ccd, encoding='latin-1').read()
tracks = {}
for m in re.finditer(r'\[TRACK (\d+)\]\s*MODE=(\d+)((?:\s*INDEX \d+=\d+)*)', text):
    idx = dict((int(a), int(b)) for a, b in re.findall(r'INDEX (\d+)=(\d+)', m.group(3)))
    tracks[int(m.group(1))] = (int(m.group(2)), idx)
total = os.path.getsize(img) // 2352
nums = sorted(tracks)
os.makedirs(out_dir, exist_ok=True)
toc = ['# track start_lba sectors (track 1 = data)']
with open(img, 'rb') as f:
    for i, t in enumerate(nums):
        mode, idx = tracks[t]
        start = idx[1]
        if i + 1 < len(nums):
            nidx = tracks[nums[i + 1]][1]
            end = nidx.get(0, nidx[1])
        else:
            end = total
        n = end - start
        toc.append('%d %d %d' % (t, start, n))
        f.seek(start * 2352)
        if mode == 1:
            with open(out_iso, 'wb') as o:
                for s in range(n):
                    o.write(f.read(2352)[16:16 + 2048])
        else:
            data = f.read(n * 2352)
            with open(os.path.join(out_dir, 'track%02d.wav' % t), 'wb') as o:
                o.write(b'RIFF' + struct.pack('<I', 36 + len(data)) + b'WAVEfmt ' +
                        struct.pack('<IHHIIHH', 16, 1, 2, 44100, 176400, 4, 16) +
                        b'data' + struct.pack('<I', len(data)) + data)
        print(t, mode, start, n)
toc.append('leadout %d' % total)
open(os.path.join(out_dir, 'toc.txt'), 'w').write('\n'.join(toc) + '\n')
