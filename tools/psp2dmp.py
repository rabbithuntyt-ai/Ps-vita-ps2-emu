#!/usr/bin/env python3
# tools/psp2dmp.py DUMP.psp2dmp [--raw]: a Vita core dump (gzip ELF core) on the PC: program
# headers, the notes (name, type, size) and, from Sony's notes, the modules (name, segment
# bases) and the threads (name, stop reason, PC, LR, SP). --raw prints the first bytes of
# each note for unknown layouts.
import gzip, struct, sys

data = gzip.open(sys.argv[1], 'rb').read()
raw = '--raw' in sys.argv
assert data[:4] == b'\x7fELF', 'not an ELF core'
e_phoff, = struct.unpack_from('<I', data, 0x1C)
e_phentsize, e_phnum = struct.unpack_from('<HH', data, 0x2A)
print('ELF core: %d bytes, %d program headers' % (len(data), e_phnum))
notes = []
loads = []
for i in range(e_phnum):
    p_type, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_flags, p_align = struct.unpack_from('<8I', data, e_phoff + i * e_phentsize)
    if p_type == 4:
        notes.append((p_offset, p_filesz))
    elif p_type == 1:
        loads.append((p_vaddr, p_offset, p_filesz))

def notes_in(offset, size):
    end = offset + size
    while offset + 12 <= end:
        namesz, descsz, ntype = struct.unpack_from('<3I', data, offset)
        name = data[offset + 12:offset + 12 + namesz].rstrip(b'\0').decode('latin1')
        desc_off = offset + 12 + ((namesz + 3) & ~3)
        yield name, ntype, data[desc_off:desc_off + descsz]
        offset = desc_off + ((descsz + 3) & ~3)

def cstr(b, off, n):
    return b[off:off + n].split(b'\0')[0].decode('latin1', 'replace')

for offset, size in notes:
    for name, ntype, desc in notes_in(offset, size):
        print('note %-16s type 0x%x size %d' % (name, ntype, len(desc)))
        if raw:
            print('   ', desc[:96].hex())
print('load segments: %d' % len(loads))

def table(desc):
    #version, count, then entries that start with their own size (all the same)
    version, count, size = struct.unpack_from('<3I', desc, 0)
    return [desc[8 + i * size:8 + (i + 1) * size] for i in range(count)]

regs = {}
modules = []
threads = []
for offset, size in notes:
    for name, ntype, desc in notes_in(offset, size):
        if ntype == 0x1003:
            for e in table(desc):
                uid, = struct.unpack_from('<I', e, 4)
                threads.append((uid, cstr(e, 8, 32), e))
        elif ntype == 0x1004:
            for e in table(desc):
                uid, = struct.unpack_from('<I', e, 4)
                regs[uid] = struct.unpack_from('<16I', e, 8)
        elif ntype == 0x1005:
            for e in table(desc):
                modules.append(e)

print('threads:')
for uid, name, e in threads:
    words = struct.unpack_from('<%dI' % (len(e) // 4), e, 0)
    r = regs.get(uid)
    extra = ''
    if r:
        extra = ' pc %08x lr %08x sp %08x' % (r[15], r[14], r[13])
    #stop reason at 0x74 (0x30004: undefined instruction, 0x30002/3: aborts)
    stop = words[0x74 // 4] if len(words) > 0x74 // 4 else 0
    print('  %08x %-28s stop 0x%x%s' % (uid, name, stop, extra))
print('modules:')
for e in modules:
    #names: the first printable run of 8+ characters
    import re
    m = re.search(rb'[\x20-\x7e]{6,}', e)
    words = struct.unpack_from('<%dI' % (len(e) // 4), e, 0)
    addrs = [w for w in words if 0x80000000 <= w < 0xF0000000 and (w & 0xFFF) == 0]
    print('  %-28s %s' % (m.group(0).decode() if m else '?', ' '.join('%08x' % a for a in addrs[:4])))
