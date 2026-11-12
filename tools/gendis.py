"""Print a recompiled function from src/gen as its x86 listing (the asm in
the comments, calls named).  usage: gendis.py <hexaddr> [...]"""
import re, sys, glob, os
here = os.path.dirname(os.path.abspath(__file__))
files = sorted(glob.glob(os.path.join(glob.escape(os.path.dirname(here)), 'src', 'gen', '*', 'gen_*.c')))
src = {f: open(f, encoding='utf-8', errors='replace').read().replace(chr(13), '').split(chr(10)) for f in files}
for a in sys.argv[1:]:
    a = a.lower().lstrip('0x') if a.startswith('0x') else a.lower()
    for f, L in src.items():
        hit = [i for i, l in enumerate(L) if re.match(r'void \w+_%s\(void\)$' % a, l)]
        if not hit: continue
        i = hit[0]
        print('==', a)
        while i < len(L) and L[i] != '}':
            l = L[i]
            m = re.search(r'/\* ([0-9a-f]+): (.*?) \*/', l)
            c = re.search(r'SYNC_OUT; (\w+)\(\)', l)
            r = re.search(r'RD32\(\(u32\)\((0x[0-9a-f]+)u\)\); esp -= 4; WR32\(esp, (0x[0-9a-f]+)u\); SYNC_OUT; rt_call', l)
            if r: print('   call [%s]' % r.group(1))
            elif c and 'esp -= 4' in l: print('   call', c.group(1).split('_')[-1])
            elif m: print('  ', m.group(1) + ':', m.group(2))
            elif re.match(r'L_[0-9a-f]+:', l.strip()): pass
            i += 1
        break
