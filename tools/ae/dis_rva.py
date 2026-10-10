# /// script
# requires-python = ">=3.11"
# dependencies = ["capstone"]
# ///
"""Disassemble a function in Fallout4.exe by RVA, to check what an id really is.

Why this exists
---------------
Two id spaces are in play and they do NOT share numbers:

  * the plugin declares ids from the **1.10.163 Address Library** (verified: every
    id in Engine.cpp resolves to exactly the address in its comment when looked up
    in version-1-10-163-0.bin);
  * the CommonLibF4RD Runtime Database numbers records in the **AE/NG space**
    (verified against version-1-11-240-0.bin).

So a name match between them proves nothing. Example that cost real time:

    Renderer::DecRef    OG id 1039714 -> RVA 0x1D0C5D0   (a large array loop)
                        AE id 2276870 -> RVA 0x181A7A0   (lock xadd [rdx+38], -1)

The AE one is `DecRef(Buffer*)` - it decrements BSGraphics::Buffer::refCount at
+0x38, exactly as the SDK declares. The OG id of the same *name* points at a
completely different function. Comparing the disassembly is the only reliable check.

Usage
-----
    uv run --no-project dis_rva.py <Fallout4.exe> 0x181A7A0
    uv run --no-project dis_rva.py <Fallout4.exe> 1039714 --og-al version-1-10-163-0.bin

`--og-al` resolves the id through a 1.10.163-style address library first, then
disassembles the function the *same* RVA lands on in the given exe - useful only
for orienting yourself, because the RVA will belong to a different function when
the executable differs.
"""
import argparse
import struct
import sys
from pathlib import Path

IMAGE_BASE = 0x140000000


def load_sections(d):
    e = struct.unpack_from("<I", d, 0x3C)[0]
    if d[e:e + 4] != b"PE\0\0":
        raise SystemExit("not a PE file")
    nsec = struct.unpack_from("<H", d, e + 6)[0]
    optsz = struct.unpack_from("<H", d, e + 20)[0]
    sec = e + 24 + optsz
    out = []
    for i in range(nsec):
        o = sec + i * 40
        name = d[o:o + 8].rstrip(b"\0").decode("latin1")
        vsize, va, rsize, raw = struct.unpack_from("<IIII", d, o + 8)
        out.append((name, va, vsize, raw, rsize))
    return out


def rva2off(sections, rva):
    for _, va, vsize, raw, rsize in sections:
        if va <= rva < va + max(vsize, rsize):
            return raw + (rva - va)
    return None


def og_al_rva(path, ident):
    raw = Path(path).read_bytes()
    n = struct.unpack_from("<Q", raw, 0)[0]
    pairs = dict(struct.iter_unpack("<QQ", raw[8:8 + n * 16]))
    return pairs.get(ident)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("exe", type=Path)
    ap.add_argument("target", help="hex RVA (0x...), or an id when --og-al is given")
    ap.add_argument("--bytes", type=int, default=96)
    ap.add_argument("--og-al", metavar="BIN", help="resolve the target as an id in this 1.10.163-style address library")
    args = ap.parse_args()

    d = args.exe.read_bytes()
    sections = load_sections(d)

    if args.og_al:
        ident = int(args.target, 0)
        rva = og_al_rva(args.og_al, ident)
        if rva is None:
            raise SystemExit(f"id {ident} not in {args.og_al}")
        print(f"id {ident} -> RVA 0x{rva:X}")
    else:
        rva = int(args.target, 16)

    off = rva2off(sections, rva)
    if off is None:
        raise SystemExit(f"RVA 0x{rva:X} is not in any mapped section")

    try:
        from capstone import Cs, CS_ARCH_X86, CS_MODE_64
    except ImportError:
        raw = d[off:off + args.bytes]
        print(f"RVA 0x{rva:X}: capstone missing, raw bytes:\n  {raw.hex()}")
        return

    md = Cs(CS_ARCH_X86, CS_MODE_64)
    for ins in md.disasm(d[off:off + args.bytes], IMAGE_BASE + rva):
        print(f"  {ins.address - IMAGE_BASE:08X}  {ins.mnemonic:<10} {ins.op_str}")
        if ins.mnemonic.startswith("ret"):
            break


if __name__ == "__main__":
    main()
