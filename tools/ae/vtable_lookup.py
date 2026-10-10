# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
"""Locate a class vtable in the running Fallout 4 executable via MSVC RTTI.

Why this exists
---------------
Porting a hook to AE needs the address of an engine function whose id is not in the
Runtime Database (see rd_probe.py / sim_init.py for what is missing). For classes
that carry MSVC RTTI, the vtable can be found from the decorated type name alone:

    ".?AV<Class>@@"  ->  TypeDescriptor  ->  CompleteObjectLocator  ->  vtable

The vtable is an array of function pointers; its slot numbers match the ones the
SDKs document in their `virtual ...  // NN` comments, so e.g. NiAVObject's
UpdateDownwardPass is slot 0x30.

Verified: BSFadeNode resolves to vtable RVA 0x2903BA8 on 1.11.240, the same value
the Runtime Database reports for REL::ID(93613).

What it does NOT solve
----------------------
Finding a *constructor*: the vtable's absolute address appears nowhere in the file
(no `lea rip-relative`, no data slot), so there is nothing to back-reference. That
needs deeper analysis or a live debugger.

Usage
-----
    uv run --no-project vtable_lookup.py <Fallout4.exe> <ClassName> [--slots N]
    uv run --no-project vtable_lookup.py <Fallout4.exe> --classes NiNode,BSFadeNode,BSTriShape
    uv run --no-project vtable_lookup.py "F:\\...\\Fallout4.exe" BSFadeNode
"""
import struct
import sys
from pathlib import Path

IMAGE_BASE = 0x140000000


class Pe:
    def __init__(self, path: Path):
        self.d = path.read_bytes()
        e = struct.unpack_from("<I", self.d, 0x3C)[0]
        if self.d[e:e + 4] != b"PE\0\0":
            raise SystemExit("not a PE file")
        nsec = struct.unpack_from("<H", self.d, e + 6)[0]
        optsz = struct.unpack_from("<H", self.d, e + 20)[0]
        opt = e + 24
        if struct.unpack_from("<H", self.d, opt)[0] != 0x20B:
            raise SystemExit("not a PE32+ image")
        self.image_base = struct.unpack_from("<Q", self.d, opt + 24)[0]
        self.sections = []
        sec = opt + optsz
        for i in range(nsec):
            o = sec + i * 40
            name = self.d[o:o + 8].rstrip(b"\0").decode("latin1")
            vsize, va, rsize, raw = struct.unpack_from("<IIII", self.d, o + 8)
            self.sections.append((name, va, vsize, raw, rsize))

    def off2rva(self, off):
        for name, va, vsize, raw, rsize in self.sections:
            if raw <= off < raw + rsize:
                return va + (off - raw), name
        return None, None

    def rva2off(self, rva):
        for name, va, vsize, raw, rsize in self.sections:
            if va <= rva < va + max(vsize, rsize):
                return raw + (rva - va)
        return None

    def find_all(self, pat: bytes):
        out, i = [], 0
        while True:
            j = self.d.find(pat, i)
            if j < 0:
                return out
            out.append(j)
            i = j + 1


def resolve(pe: Pe, class_name: str, slots: int = 8):
    name = f".?AV{class_name}@@".encode()
    hits = pe.find_all(name)
    if not hits:
        return None, "type name not found"
    results = []
    for name_off in hits:
        # MSVC TypeDescriptor: { void* pVFTable; void* spare; char name[]; }
        td_off = name_off - 16
        td_rva, _ = pe.off2rva(td_off)
        # CompleteObjectLocator references the TypeDescriptor RVA.
        col_candidates = pe.find_all(struct.pack("<I", td_rva))
        for col_off in col_candidates:
            col_rva, _ = pe.off2rva(col_off)
            # The COL starts 8 bytes before its self-rva field inside it; use the
            # reference that points at a plausible COL (signature 1, self-rva == col_rva).
            for base in range(col_off - 32, col_off + 1, 4):
                sig, off_, cd, td, cdrva, self_rva = struct.unpack_from("<6I", pe.d, base)
                b_rva, _ = pe.off2rva(base)
                if sig == 1 and td == td_rva and self_rva == b_rva:
                    col_va = pe.image_base + b_rva
                    vt_refs = pe.find_all(struct.pack("<Q", col_va))
                    for vt_ref in vt_refs:
                        vt_off = vt_ref + 8
                        vt_rva, vt_sec = pe.off2rva(vt_off)
                        ptrs = struct.unpack_from(f"<{slots}Q", pe.d, vt_off)
                        results.append({
                            "type_name_off": name_off,
                            "td_rva": td_rva,
                            "col_rva": b_rva,
                            "vtable_rva": vt_rva,
                            "section": vt_sec,
                            "slots": [p - pe.image_base for p in ptrs],
                        })
                    break
    if not results:
        return None, "vtable reference not found"
    return results, None


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    pe = Pe(Path(sys.argv[1]))
    slots = 8
    if "--slots" in sys.argv:
        slots = int(sys.argv[sys.argv.index("--slots") + 1])
    if "--classes" in sys.argv:
        names = sys.argv[sys.argv.index("--classes") + 1].split(",")
        for n in names:
            res, err = resolve(pe, n, slots)
            if err:
                print(f"{n:<28} {err}")
                continue
            r = res[0]
            print(f"{n:<28} vtable RVA 0x{r['vtable_rva']:X} ({r['section']})")
        return
    res, err = resolve(pe, sys.argv[2], slots)
    if err:
        raise SystemExit(f"{sys.argv[2]}: {err}")
    for r in res:
        print(f"{sys.argv[2]}: vtable RVA 0x{r['vtable_rva']:X} ({r['section']})")
        print(f"  TypeDescriptor 0x{r['td_rva']:X}, CompleteObjectLocator 0x{r['col_rva']:X}")
        for i, s in enumerate(r["slots"]):
            print(f"  slot 0x{i:02X}: rva 0x{s:X}")


if __name__ == "__main__":
    main()
