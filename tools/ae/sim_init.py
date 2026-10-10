# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
"""Offline simulation of Engine::Init()'s capability probe on a given runtime.

Reads the CommonLibF4RD Runtime Database (via rd_probe.py, which is in this
directory) and, for every id Engine.cpp asks for, works out what
`REL::ID::id(version)` returns and whether the database resolves it. No game start
and no F4SE needed.

How the lookup works (REL::ID in CommonLibF4RD/CommonLibF4/include/REL/Relocation.h)
-----------------------------------------------------------------------------------
A declaration carries up to three slots, always in the order OG, NG, AE:

    REL::ID{og, INVALID_ID}      // Engine.h: an OG-only id, AE slot left empty
    REL::ID{a, b}                // two-argument form: OG, AE (NG shares the AE value)
    REL::ID(n, n)                // VTABLE_IDs.h: the same number on every family
    REL::ID{n}                   // AE slot only

`id(version)` picks the running family's slot, falling back to the AE slot when the
family's own slot is INVALID_ID (0xFFFFFFFFFFFFFFFF).

Why a miss is safe
------------------
`REL::ID::address()` is fatal: it calls `stl::report_and_fail` (an error box that
stops the game) when the id does not resolve. `Engine.cpp` therefore calls
`REL::IDDatabase::get().resolve(id)` instead, which returns a result object with an
optional RVA, and skips the lookup entirely when `id(version)` is INVALID_ID. A
missing id costs a capability, never the session.

Result on AE (1.11.240)
---------------------
Offline (this script) and on a live start (2026-10-11, log path below) agree:

    23 engine ids miss  -> the OG-only function/global ids (Engine.cpp "Ask" list)
     6 VTABLEs resolve  -> BSTriShape, BSMeshLODTriShape, NiNode, BSFadeNode,
                           BSLightingShaderProperty, BSEffectShaderProperty
     extra ids resolve  -> TES::Singleton, GridCellArray::Get,
                           GameSettingCollection::Singleton
    CombineReady() == false -> Plugin.cpp logs and installs nothing

The live log (D:\UserData\F4SE_log\RuntimeCombiner.log on the porting machine)
reads:

    Runtime Combiner v0.3.1 loading: game 1-11-240-0, F4SE 0-7-9-0
    settings (RuntimeCombiner.ini): enabled true, ...
    23 engine ids are missing on 1-11-240-0: NiObject::Clone, ...
    engine ids: combine incomplete, previs missing, precombines available -> combining disabled
    combining is not enabled on 1-11-240-0: its engine ids are verified for
      1.10.163 (OG) only; the plugin loads, reads RuntimeCombiner.ini and
      changes nothing in the scene
    precombine switch: available (bDisablePrecombines would work once combining is enabled)

Six lines for the whole start, no error box: the fatal path
(REL::ID::address() -> stl::report_and_fail) is never reached.

Class layouts are NOT the blocker
---------------------------------
Every Offset:: constant in Engine.h was checked against the AE-only CommonLibF4
headers (D:\Sou\Fallout_4\CommonLibF4\include\RE) and all of them match the OG
values, field for field:
    BSGeometry  modelBound 0x120, properties[2] 0x130, skinInstance 0x140,
                rendererData 0x148, vertexDesc 0x150, type 0x158
    BSTriShape  numTriangles 0x160, numVertices 0x164
    NiAVObject  fadeAmount 0x118, multType 0x11C (= kFadeType)
    BSFadeNode  nearDistSqr 0x198, farDistSqr 0x19C, currentFade 0x1A0,
                frameCounter 0x1B0; sizeof == 0x1C0
    BSShaderProperty  alpha 0x28; CanMerge is vtable slot 0x31 (= kCanMergeSlot)
    BSGraphics::Buffer  data 0x08, dataSize 0x34, pendingRequests 0x44
    BSGraphics::TriShape  vertexBuffer 0x08, indexBuffer 0x10
So porting is an *id* problem, not a layout problem. Re-verify anyway before
trusting any of it on a live runtime.

Usage
-----
    uv run --no-project sim_init.py <f4rd-runtime.bin> [major.minor.patch.build]

The version argument defaults to 1.11.240.0.
"""
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import rd_probe  # reuse the reader

INVALID_ID = 0xFFFFFFFFFFFFFFFF


def family(v):
    return "AE" if v >= (1, 11, 0, 0) else ("NG" if v >= (1, 10, 980, 0) else "OG")


def slot(og, ae, target):
    """REL::ID::id(version): the running family's slot, falling back to the AE slot."""
    fam = family(target)
    if fam == "OG":
        return og if og != INVALID_ID else ae
    if fam == "NG":
        return ae  # NG slot shares the AE value in this codebase
    return ae


# (name, REL::ID(og, ae), capability group, source)
ENGINE_IDS = [
    ("NiObject::Clone",                (604942,   INVALID_ID), "clone"),
    ("Renderer::CreateTriShape",       (99624,    INVALID_ID), "triShape"),
    ("Renderer::DecRef(TriShape)",     (1039714,  INVALID_ID), "triShape"),
    ("Renderer",                       (1378294,  INVALID_ID), "triShape"),
    ("BSGeometry::SetRendererData",    (444114,   INVALID_ID), "geometry"),
    ("NiNode::NiNode",                 (20633,    INVALID_ID), "nodes"),
    ("BSFadeNode::BSFadeNode",         (668955,   INVALID_ID), "nodes"),
    ("ConfigureFadeNodeRange",         (1417061,  INVALID_ID), "fade"),
    ("uGridsToLoad",                   (504589,   INVALID_ID), "fade"),
    ("fade frame number",              (734919,   INVALID_ID), "fade"),
    ("NiAVObject::Update",             (121052,   INVALID_ID), "update"),
    ("TES",                            (1194835,  2698044),   "cells"),
    ("GridCellArray::Get",             (1330136,  2194566),   "cells"),
    ("bUseCombinedObjects",            (267057,   INVALID_ID), "precombines"),
    # The GameSettingCollection fallback for the same switch: RE::GameSettingCollection::GetSingleton()
    # (declared in the SDK as REL::ID(8308, 4797590)). It resolves on every family, so caps.precombines
    # is true on AE even though the OG global's id is not.
    ("GameSettingCollection::Singleton", (8308,  4797590),    "precombines"),
    ("previs query",                   (652211,   INVALID_ID), "previs"),
    ("previs active query",            (917969,   INVALID_ID), "previs"),
    ("BSFadeNode::SetRange",           (1164193,  INVALID_ID), "fade"),
    ("fade multipliers",               (804710,   INVALID_ID), "fade"),
    ("fDistanceMultiplier",            (515159,   INVALID_ID), "fade"),
    ("fading enabled",                 (1220201,  INVALID_ID), "fade"),
    ("Render_PreUI",                   (984743,   INVALID_ID), "previsHook"),
    ("previs query (site)",            (1264353,  INVALID_ID), "previsHook"),
    ("MultiCellVisibilityData",        (787073,   INVALID_ID), "previsHook"),
    ("RegisterDynamicObject",          (272586,   INVALID_ID), "previsHook"),
    ("UnregisterDynamicObject",        (697930,   INVALID_ID), "previsHook"),
    ("Main::PerformGameReset",         (124452,   INVALID_ID), "resetHook"),
    ("TES::PurgeBufferedCells",        (1075115,  INVALID_ID), "resetHook"),
    ("VTABLE::BSTriShape",             (183326,   183326),    "triShape"),
    ("VTABLE::BSMeshLODTriShape",      (403494,   403494),    "triShape"),
    ("VTABLE::NiNode",                 (1048271,  1048271),   "nodes"),
    ("VTABLE::BSFadeNode",             (93613,    93613),     "nodes"),
    ("VTABLE::BSLightingShaderProperty", (241915, 241915),    "shader"),
    ("VTABLE::BSEffectShaderProperty", (708622,   708622),    "shader"),
]

COMBINE_NEEDS = ["clone", "triShape", "geometry", "nodes", "update", "fade", "cells"]

# Groups reached by whichever of their ids resolves first (Engine.cpp checks both).
# bUseCombinedObjects: the OG global, or the GameSettingCollection entry of the same name.
ANY_OF = ["precombines"]

# Groups that no longer need any engine id, because Engine.cpp uses a virtual call or an inline SDK
# constructor instead (the vtable slots and class layouts are the same on every runtime):
#   update  - NiAVObject::Update is inlined everywhere; UpdateStatic calls the virtual passes
#             UpdateDownwardPass (slot 0x30) and UpdateUpwardPass (slot 0x42) directly.
NO_ID_NEEDED = ["update"]


def main():
    db = rd_probe.Db(Path(sys.argv[1]))
    target = tuple(int(x) for x in (sys.argv[2] if len(sys.argv) > 2 else "1.11.240.0").split("."))
    target = (target + (0, 0, 0, 0))[:4]
    print(f"# simulating Engine::Init() on {family(target)} {target}")

    by_id = {}
    for r in db.all_records():
        by_id[r["id"]] = r

    groups = {}
    for name, (og, ae), group in ENGINE_IDS:
        rid = slot(og, ae, target)
        if rid == INVALID_ID:
            status, rva = "no id for this runtime (skipped)", None
        else:
            rec = by_id.get(rid)
            if rec is None:
                status, rva = f"id {rid}: not in database", None
            else:
                rva = None
                for k in range(rec["known_count"]):
                    kn = db.known(rec["first_known"] + k)
                    if rd_probe.scope_match(kn["version"], target, kn["flags"] & 1):
                        rva = kn["rva"]
                        break
                status = f"id {rid}: resolved RVA {rva:#x}" if rva is not None else f"id {rid}: no {family(target)} entry"
        ok = rva is not None
        groups.setdefault(group, []).append(ok)
        print(f"  [{'OK ' if ok else 'MISS'}] {name:<36} {status}")

    print("\n# capability groups")
    caps = {}
    for g, v in groups.items():
        if g in NO_ID_NEEDED:
            caps[g] = True
        elif g in ANY_OF:
            caps[g] = any(v)
        else:
            caps[g] = all(v)
    for g in NO_ID_NEEDED:
        print(f"  ({g}: ready, no engine id needed)")
    if "precombines" in groups:
        print(f"  (precombines: any-of {sum(groups['precombines'])}/{len(groups['precombines'])} resolved)")
    for g in sorted(groups):
        print(f"  {g:<12} {'ready' if caps[g] else 'INCOMPLETE'}")
    ready = all(caps.get(n, False) for n in COMBINE_NEEDS)
    print(f"\nCombineReady() = {ready}")
    if not ready:
        print("  missing: " + ", ".join(n for n in COMBINE_NEEDS if not caps.get(n, False)))


if __name__ == "__main__":
    main()
