#!/usr/bin/env python3
# tools/vitasym.py ELF ADDR...: the function (from nm) containing each address of the Vita program
import subprocess, sys, bisect
elf = sys.argv[1]
out = subprocess.run(['/usr/local/vitasdk/bin/arm-vita-eabi-nm', '-C', '-n', elf], capture_output=True, text=True).stdout
syms = []
for line in out.splitlines():
    parts = line.strip().split(' ', 2)
    if len(parts) < 3 or len(parts[0]) != 8 or parts[1] not in 'tTwW':
        continue
    syms.append((int(parts[0], 16), parts[2]))
addrs = [s[0] for s in syms]
for a in sys.argv[2:]:
    v = int(a, 16) & ~1
    i = bisect.bisect_right(addrs, v) - 1
    if i >= 0:
        print('%08x  %s+0x%x' % (v, syms[i][1], v - syms[i][0]))
    else:
        print('%08x  ?' % v)
