# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
"""Offline reader for the CommonLibF4RD Runtime Database (f4rd-runtime.bin).

Why this exists
---------------
Porting the plugin to another Fallout 4 runtime (AE / NG) needs to know, for every
engine id the plugin asks for, whether the running executable can still resolve it.
That answer lives in the Runtime Database, and this script reads it without starting
the game.

Where the database is
---------------------
    <MO2 instance>/mods/Runtime Database/F4SE/Plugins/f4rd-runtime.bin
On the game side it must sit at `Data/F4SE/Plugins/f4rd-runtime.bin`.
The distributed file carries a legacy prefix: a table of (version_index,
byte_offset) pairs, then the real F4RD database. Its length is not self-describing,
so the magic (b"F4RDBIN\0", no space) is located by searching the file.

File format (see CommonLibF4RD/CommonLibF4/src/REL/RuntimeDatabase.cpp)
----------------------------------------------------------------------
Header (112 bytes at the magic):
     8  u16 formatMajor (1)
    10  u16 formatMinor
    12  u32 headerSize (112)
    20  u32 endian marker (0x01020304)
    24  u32 recordCount
    28  u32 aliasCount
    32  u32 knownCount
    36  u32 candidateCount
    40  u32 fragmentCount
    48  u64 recordsOffset
    56  u64 aliasesOffset
    64  u64 knownOffset
    72  u64 candidatesOffset
    80  u64 fragmentsOffset
    96  u64 total size
Records (40 bytes each):
     0  u64 id (canonical; the AE/NG numbering space)
     8  u32 firstAlias
    12  u32 aliasCount
    16  u32 firstKnown
    20  u32 knownCount
    24  u32 firstCandidate
    28  u32 candidateCount
    32  u32 flags
Aliases (24 bytes each):
     0  u64 id
     8  u16[4] version
    16  u32 recordIndex
    20  u32 flags  (bit 0 = version scope is major.minor)
Known RVAs (16 bytes each):
     0  u16[4] version
     8  u32 rva
    12  u32 flags  (bit 0 = version scope is major.minor)

What the numbers mean
---------------------
* A record's `id` is in the **AE/NG numbering space**, not the classic 1.10.163
  Address Library numbering. Verified by cross-checking
  `TESFullName::LoadFullNameChunk = 2193215 -> RVA 0x315FB0` against F4LoadText's
  notes for 1.11.240.
* Only ~21k of ~782k records carry an OG (1.10.163) RVA, and all of those also
  carry an AE one. OG-only ids (the ones this plugin declares as
  `REL::ID{og, INVALID_ID}`) are simply **not present**, neither as a canonical id
  nor as an alias -- there is no alias bridge to fall back on.

Usage
-----
    uv run --no-project rd_probe.py <f4rd-runtime.bin> [id ...]

With no ids it walks every record (slow, ~782k lines). With ids it prints, for
each one, the canonical record (if any), its known RVAs per version family and its
aliases. An id that is absent from both tables prints "NOT IN DATABASE".
"""
import struct
import sys
from pathlib import Path

MAGIC = b"F4RDBIN\x00"
HEADER_SIZE = 112
RECORD_SIZE = 40
ALIAS_SIZE = 24
KNOWN_RVA_SIZE = 16

ALIAS_VERSION_MAJOR_MINOR = 1  # alias flags bit 0
KNOWN_FLAG_VERSION_MAJOR_MINOR = 1  # known-RVA flags bit 0 (same convention)


class Db:
    def __init__(self, path: Path):
        self.raw = path.read_bytes()
        off = self._find_db_start()
        self.b = memoryview(self.raw)[off:]
        if self.b[:8] != MAGIC:
            raise SystemExit("bad magic")
        if struct.unpack_from("<H", self.b, 8)[0] != 1:
            raise SystemExit("unsupported major")
        self.format_minor = struct.unpack_from("<H", self.b, 10)[0]
        self.record_count = struct.unpack_from("<I", self.b, 24)[0]
        self.alias_count = struct.unpack_from("<I", self.b, 28)[0]
        self.known_count = struct.unpack_from("<I", self.b, 32)[0]
        self.candidate_count = struct.unpack_from("<I", self.b, 36)[0]
        self.fragment_count = struct.unpack_from("<I", self.b, 40)[0]
        self.records_offset = struct.unpack_from("<Q", self.b, 48)[0]
        self.aliases_offset = struct.unpack_from("<Q", self.b, 56)[0]
        self.known_offset = struct.unpack_from("<Q", self.b, 64)[0]

    def _find_db_start(self) -> int:
        # The distributed file carries a legacy prefix: a table of
        # (version_index, byte_offset) pairs, then the F4RD database.
        # Locate the magic directly; the prefix length is not self-describing.
        i = self.raw.find(MAGIC)
        if i < 0:
            raise SystemExit("magic not found")
        return i

    # record: u64 id, u32 firstAlias, u32 aliasCount, u32 firstKnown, u32 knownCount,
    #         u32 firstCandidate, u32 candidateCount, u32 flags, u32 pad
    def record(self, i: int):
        o = self.records_offset + i * RECORD_SIZE
        (rid, first_alias, na, first_known, nk,
         first_cand, nc, flags, pad) = struct.unpack_from("<QIIIIIIII", self.b, o)
        return dict(idx=i, id=rid, first_alias=first_alias, alias_count=na,
                    first_known=first_known, known_count=nk,
                    candidate_count=nc, flags=flags)

    # alias: u64 id, u16[4] version, u32 recordIndex, u32 flags
    def alias(self, i: int):
        o = self.aliases_offset + i * ALIAS_SIZE
        aid = struct.unpack_from("<Q", self.b, o)[0]
        ver = struct.unpack_from("<4H", self.b, o + 8)
        rec = struct.unpack_from("<I", self.b, o + 16)[0]
        flags = struct.unpack_from("<I", self.b, o + 20)[0]
        return dict(id=aid, version=ver, record=rec, flags=flags)

    # known RVA: u16[4] version, u32 rva, u32 flags
    def known(self, i: int):
        o = self.known_offset + i * KNOWN_RVA_SIZE
        ver = struct.unpack_from("<4H", self.b, o)
        rva = struct.unpack_from("<I", self.b, o + 8)[0]
        flags = struct.unpack_from("<I", self.b, o + 12)[0]
        return dict(version=ver, rva=rva, flags=flags)

    def all_records(self):
        for i in range(self.record_count):
            yield self.record(i)

    def _dump_record(self, r, target, alias_by_id, recs_by_id):
        for k in range(r["known_count"]):
            kn = self.known(r["first_known"] + k)
            mark = "  <= 1.11.240" if scope_match(kn["version"], target, kn["flags"] & 1) else ""
            print(f"   known RVA {kn['rva']:#010x}  {family(kn['version'])} {kn['version']} "
                  f"flags=0x{kn['flags']:X}{mark}")
        for k in range(r["alias_count"]):
            a = self.alias(r["first_alias"] + k)
            print(f"   alias id={a['id']}  {family(a['version'])} {a['version']} "
                  f"flags=0x{a['flags']:X}")


def family(v):
    if v >= (1, 11, 0, 0):
        return "AE"
    if v >= (1, 10, 980, 0):
        return "NG"
    return "OG"


def scope_match(src, tgt, major_minor):
    if major_minor:
        return src[0] == tgt[0] and src[1] == tgt[1] and family(src) == family(tgt)
    return tuple(src) == tuple(tgt)


def main():
    if len(sys.argv) < 2:
        raise SystemExit("usage: rd_probe.py <f4rd-runtime.bin> [id...]")
    db = Db(Path(sys.argv[1]))
    print(f"records={db.record_count} aliases={db.alias_count} "
          f"known={db.known_count} candidates={db.candidate_count} "
          f"fragments={db.fragment_count} formatMinor={db.format_minor}")

    target = (1, 11, 240, 0)
    wanted = [int(x, 0) for x in sys.argv[2:]]

    # index aliases by id
    alias_by_id: dict[int, list] = {}
    for i in range(db.alias_count):
        a = db.alias(i)
        alias_by_id.setdefault(a["id"], []).append(a)

    ids = wanted if wanted else [r["id"] for r in db.all_records()]
    recs_by_id: dict[int, list] = {}
    for r in db.all_records():
        recs_by_id.setdefault(r["id"], []).append(r)

    target = (1, 11, 240, 0)
    for rid in ids:
        recs = recs_by_id.get(rid, [])
        als = alias_by_id.get(rid, [])
        if not recs and not als:
            print(f"\nID {rid}: NOT IN DATABASE")
            continue
        for r in recs:
            print(f"\nID {r['id']} (canonical record #{r['idx']}, flags=0x{r['flags']:X}, "
                  f"candidates={r['candidate_count']})")
            db._dump_record(r, target, alias_by_id, recs_by_id)
        if not recs and als:
            print(f"\nID {rid}: only an ALIAS id (no canonical record)")
        for a in als:
            rec = db.record(a["record"])
            print(f"   alias {a['id']}  {family(a['version'])} {a['version']} "
                  f"flags=0x{a['flags']:X}  -> canonical #{a['record']} (id {rec['id']})")
            db._dump_record(rec, target, alias_by_id, recs_by_id)


if __name__ == "__main__":
    main()
