#!/usr/bin/env python3
"""Decodes a VIF1 DMA source chain from an EE RAM dump: DMA tags and the VIF
commands they carry, with a summary (unpacked bytes, MSCALs, DIRECT data).

    tools/vif_chain.py eeram.bin START_TAG_ADDRESS [--max-tags N] [--quiet]

The start address is a DMA tag (D1_TADR when the chain started, or the start
of a chain the game builds). Debugging aid for the PC runner's --dump-ram.
"""
import struct
import sys

TAG_IDS = ["refe", "cnt", "next", "ref", "refs", "call", "ret", "end"]
UNPACK_FORMATS = {0x0: ("S-32", 4), 0x1: ("S-16", 2), 0x2: ("S-8", 1), 0x4: ("V2-32", 8), 0x5: ("V2-16", 4),
                  0x6: ("V2-8", 2), 0x8: ("V3-32", 12), 0x9: ("V3-16", 6), 0xA: ("V3-8", 3), 0xC: ("V4-32", 16),
                  0xD: ("V4-16", 8), 0xE: ("V4-8", 4), 0xF: ("V4-5", 2)}


def main():
    ram = open(sys.argv[1], "rb").read()
    tadr = int(sys.argv[2], 16) & 0x01FFFFFF
    max_tags = 64
    quiet = "--quiet" in sys.argv
    if "--max-tags" in sys.argv:
        max_tags = int(sys.argv[sys.argv.index("--max-tags") + 1])
    word = lambda a: struct.unpack_from("<I", ram, a & 0x01FFFFF0 | (a & 0xC))[0]

    # The VIF stream continues across DMA packets: collect it, then parse.
    stream = []  # (address, word)
    stack = []
    for _ in range(max_tags):
        lo, hi = word(tadr), word(tadr + 4)
        qwc, tid, addr = lo & 0xFFFF, (lo >> 28) & 7, hi & 0x01FFFFFF
        if not quiet:
            print("tag %08X: %-4s qwc %4u addr %08X | vif %08X %08X" % (tadr, TAG_IDS[tid], qwc, addr, word(tadr + 8), word(tadr + 12)))
        # The tag's upper 64 bits go to the VIF when TTE is off: VIF codes too
        # (games put NOPs or STCYCL/UNPACK headers there).
        stream += [(tadr + 8, word(tadr + 8)), (tadr + 12, word(tadr + 12))]
        if tid in (1, 2, 5):  # data follows the tag
            data = tadr + 16
        else:
            data = addr
        stream += [(data + i * 4, word(data + i * 4)) for i in range(qwc * 4)]
        if tid == 0 or tid == 7:  # refe/end
            break
        if tid == 1:  # cnt: next tag after the data
            tadr = data + qwc * 16
        elif tid == 2:  # next
            tadr = addr
        elif tid in (3, 4):  # ref/refs: next tag follows this one
            tadr += 16
        elif tid == 5:  # call
            stack.append(data + qwc * 16)
            tadr = addr
        elif tid == 6:  # ret
            if not stack:
                break
            tadr = stack.pop()

    summary = {}
    i = 0
    while i < len(stream):
        a, w = stream[i]
        cmd, num, imm = (w >> 24) & 0x7F, (w >> 16) & 0xFF, w & 0xFFFF
        i += 1
        if cmd >= 0x60:  # UNPACK
            fmt = cmd & 0xF
            name, size = UNPACK_FORMATS.get(fmt, ("?", 4))
            n = num or 256
            words = (n * size + 3) // 4
            key = "UNPACK %s" % name
            summary[key] = summary.get(key, 0) + n * size
            if not quiet:
                print("  %08X UNPACK %-5s num %3u addr %03X%s" % (a, name, n, imm & 0x3FF, " +tops" if imm & 0x8000 else ""))
            i += words
        elif cmd in (0x50, 0x51):  # DIRECT / DIRECTHL
            n = (imm or 65536)
            summary["DIRECT qw"] = summary.get("DIRECT qw", 0) + n
            if not quiet:
                print("  %08X DIRECT%s %u qw" % (a, "HL" if cmd == 0x51 else "", n))
            i = (i + 3) // 4 * 4 + n * 4  # data is qword aligned
        elif cmd == 0x4A:  # MPG
            n = num or 256
            if not quiet:
                print("  %08X MPG %u instructions at %03X" % (a, n, imm))
            i = (i + 1) // 2 * 2 + n * 2
        elif cmd in (0x14, 0x15, 0x17):  # MSCAL / MSCALF / MSCNT
            key = "MSCAL %03X" % (imm * 8) if cmd != 0x17 else "MSCNT"
            summary[key] = summary.get(key, 0) + 1
            if not quiet:
                print("  %08X %s" % (a, key))
        elif cmd in (0x30, 0x31):  # STROW / STCOL
            i += 4
        elif cmd == 0x20:  # STMASK
            i += 1
        elif cmd != 0 and not quiet:
            print("  %08X VIF %02X num %u imm %04X" % (a, cmd, num, imm))
    print("summary:", ", ".join("%s %s" % (k, v) for k, v in sorted(summary.items())))


if __name__ == "__main__":
    main()
