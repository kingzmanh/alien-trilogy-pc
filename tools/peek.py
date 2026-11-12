"""peek.py <exe> <hex addr> [len]: bytes + text at a relocated address (LE_RELOC honoured)"""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from le import LE
l = LE(sys.argv[1]); a = int(sys.argv[2], 16); n = int(sys.argv[3]) if len(sys.argv) > 3 else 64
b = bytes(l.u8(a + i) for i in range(n))
print(b.hex(' ')); print(b.replace(b'\0', b'|').decode('latin-1'))
