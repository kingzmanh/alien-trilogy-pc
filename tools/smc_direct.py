"""Find self-modifying code that writes straight into the flat code object
(flat model: ds covers the code too, no alias selector needed): every
instruction whose memory destination is an absolute address inside an
executable object.  Groups the writes by the function doing them and the
function (from functions.tsv) the patched bytes belong to.
usage: smc_direct.py GAME.EXE functions.tsv"""
import sys, os, bisect, re
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from x86an import Prog

p = Prog(sys.argv[1])
fs = sorted(int(l.split('\t')[0], 16) for l in open(sys.argv[2]) if l.strip())
WR = ('mov', 'add', 'sub', 'or', 'and', 'xor', 'inc', 'dec', 'neg', 'not', 'adc', 'sbb', 'shl', 'shr', 'sar',
      'rol', 'ror', 'stosb', 'stosd', 'movsb', 'xchg', 'pop', 'fstp', 'fst', 'fistp', 'setne', 'sete')
def owner(a):
    i = bisect.bisect_right(fs, a) - 1
    return fs[i] if i >= 0 else 0
seen = {}
a = p.clo
while a < p.chi:
    ins = p.insn(a)
    if ins is None:
        a += 1
        continue
    if ins.mnemonic in WR and ins.op_str.split(',')[0].find('ptr [0x') >= 0:
        m = re.search(r'ptr \[0x([0-9a-f]+)\]', ins.op_str.split(',')[0])
        if m:
            t = int(m.group(1), 16)
            if p.clo <= t < p.chi:
                seen.setdefault((owner(a), owner(t)), []).append((a, t))
    a += ins.size
for (w, t), l in sorted(seen.items()):
    print('writer %x -> code of %x: %d stores, targets %s' % (w, t, len(l), ' '.join('%x' % x for _, x in l[:12])))
