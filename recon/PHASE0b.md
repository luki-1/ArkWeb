# Phase 0b: runtime recon findings

## Arkham Knight, session 1 (2026-10-01, read-only DLL)

Exe base that run: `7FF68DA70000`. All addresses below are RVAs.

**GNames** is `TArray<FNameEntry*>` at `exe+3a208b8` (229,201 names in Gotham free roam). The
static guess `3a09f30` was wrong.
- `FNameEntry` (pack 4):
  - `u32 index<<2 | flags` at +0x0
  - `FNameEntry* hashNext` at +0x4
  - name at +0xC
- Flags:
  - bit0: UTF-16 inline
  - bit1: pointer to an ANSI string (the hardcoded names point into the exe's `.rdata`)
  - neither: ANSI inline
- Decoded offline into `ak_names.txt` (228,250 names, 0 failures) by `tools/ak_decode_names.py`.

**GObjObjects is not a TArray.** It is a static inline array, `UObject*[785000]`, at
`exe+340cbe4` (4-byte aligned). `Num` is at `exe+3a09f28` and `Max` (785000) at `exe+3a09f2c`. That
session held 659k objects.

**UObject** (pack 4):

| Offset | Field |
|---|---|
| +0x00 | vtable |
| +0x08 | ObjectFlags (u64) |
| +0x10..0x2F | hash links and linker data |
| +0x30 | Index (int) |
| +0x34 | Outer (UObject*) |
| +0x3C | Name (FName: index, number) |
| +0x44 | Class (UClass*) |
| +0x4C | ObjectArchetype |

`sizeof(UObject)` is 0x54 (the UClass `Object` reports PropertySize 0x54).
- Objects 0..3 are `TextBuffer`, `Object`, `System`, `Subsystem` (UClasses in Core).

**UField / UStruct** (from UClass `Object`):

| Offset | Field |
|---|---|
| +0x54 | Next |
| +0x64 | Children |
| +0x78 | u16 PropertySize, followed by u16 0x0004 (min alignment?) |

**UFunction** (first child of `Object`):
- FriendlyName FName at +0xAC
- native function pointer at +0xBC

**PxPhysics** is at `7B36490`, vtable `PhysX3_x64+27dd90` (48 slots logged in `ak_recon.log`).

The v2 DLL adds `objlist`, `inst`, `find` and `name`, which use these layouts. Full class, struct
and property layouts come from `ak_fields.bin` offline.

## Arkham Knight, session 2 (v2 DLL), complete

- `objlist` covered 648,201 live objects and 183,606 field objects. `tools/ak_sdk.py` turns them
  into **`recon/ak_sdk.txt`**: 6,705 classes and structs with property offsets and types, plus
  functions with parameters and native pointers.
- **UProperty** (pack 4):

  | Offset | Field |
  |---|---|
  | +0x5C | ArrayDim |
  | +0x60 | PropertyFlags (u64) |
  | +0x68 | ElementSize (u16) |
  | +0x6A | Offset (u16) |
  | +0x94 | type data (Struct / PropertyClass / Inner / Enum / bool mask) |
  | +0x9C | ClassProperty MetaClass |

  Parameter flags are Rocksteady-shuffled: parm `0x8`, out `0x20`, return `0x80` (ReturnValue
  is `0xa8`).
- **UStruct:** SuperField +0x5C, Children +0x64, PropertySize u16 +0x78. **UFunction:**
  FunctionFlags +0xA4, native pointer +0xBC.

**Key live objects** (Gotham free roam):

| Object | Class | Notes |
|---|---|---|
| `BatEntry.TheWorld.PersistentLevel.RPawnPlayerBm_0` | RPawnPlayerBm, size 0x2730+ | Location +0xC4, Rotation +0xD0, Physics +0x11B (1 = walking), Velocity +0x1BC, CylinderComponent +0x384, EyeHeight +0x464 |
| `R3rdPersonCamera_1` | Engine.Camera | `CameraCache.POV` at +0x574: Location, Rotation +0x580, FOV +0x58C. That run sat about 1.8 m behind and above Batman with FOV 80. |

**Units**
- Batman's cylinder is 95 UU half-height × 40 UU radius, so he is about 190 UU tall: **1 UU ≈ 1 cm**.
- Spider-Man meters × 100 = UU. UE3 is Z-up, and Rotator 65536 = 360°.
- At that moment Batman stood at (-68928, -57263, 3894) UU.

**Traversal markers loaded around the player** (they stream with the world): 187 `RGrapplePoint`,
40 `RGrapplePointCollection`, 38 `RShimmyEdgeMarker`, 37 `RLedgeSetup`, 7 `RHidePoint_OWGargoyle`.
Raw dumps are in `logs/ak_*.bin` (location at +0xC4).

**Worlds:** 109 streamed `Level`/`World` objects.

**PhysX:** the PxPhysics vtable slots are thunks (`mov rcx,[global]; jmp impl`). The scene list
will be pinned in Phase 2; the alternative is UE3's own `RBPhysScene` from WorldInfo.

## Spider-Man, session 1 (passive hook DLL), complete

Exe base that run: `7FF75CD40000`. Free roam, starting on a rooftop.

**The three hooks are confirmed live.** All three prologue signatures matched, and the game ran
stable for 30+ minutes with the hooks in place.
- **100% of queries hit the main world** `[exe+78939e8]`.
- The only callers are the predicted game-side paths: async `Execute` (`exe+18180fe` ray,
  `exe+1818211` shape, `exe+1818312` closest) plus the immediate `exe+18173d1` (ray),
  `exe+18166df` (shape) and `exe+1816e7f` (closest). The Havok-internal callers never fired.
- Async queries run on worker threads (job system `exe+2b2ef03`/`2b2efc4`), so the Phase 2 hooks
  must be thread-safe (the recon hooks already are).

**Query load**

| Situation | Rays/s | Shape casts/s | Closest-point/s |
|---|---|---|---|
| Standing on a roof | ~9k | ~1k | ~0 |
| Swinging | ~22k | ~4.5k (peak 5.7k) | ~130 |

This is the budget the Gotham collision answerer must meet.

**hknpRayCastQuery** (from samples):

| Offset | Field |
|---|---|
| +0x00 | filter pointer |
| +0x14 | collisionFilterInfo (`0x0478xxxx`) |
| +0x30 | ray origin (vec4) |
| +0x40 | direction × length (vec4, w=1) |
| +0x50 | 1/direction |

Ground probes cast (0, -3, 0).

**Coordinate system:** **Y-up, meters**, right-handed with yaw about Y.

**Hero**
- The `Hero::HeroLocal` component (vtable `exe+38a93c8`) holds the actor record at +0x8. The
  record's +0x0 points to the world transform: rotation rows at +0x00/+0x10/+0x20 and position at
  +0x30 (vec4).
- Bounds at +0x40 (center) and +0x50 (half extents? 1.03 × 1.78 × 1.03 m).
- Other components (HeroMoverManager, HeroCameraManager, HeroRopeManager, HeroIKManager, ...)
  share the same record pointer at +0x8.
- The camera manager holds a point near the hero at +0x80.

**Swing recording:** `recon/sm_swing_trace_1hz.csv`, 240 s at 1 Hz with full transforms.
- 6.95 km path, 30 m/s average, **58 m/s peak**, height 3..135 m.
- This is the replay source for `fake_spidey` in Phase 1, and it sets the collision streaming
  radius. At about 60 m/s with anchors 30-60 m ahead, plan for at least 250 m.

**Behaviour notes**
- **The game pauses on focus loss.** Query traffic fell to 0 while it was in the background, so
  the hidden guest needs the fake-focus hook (as planned).
- The full-heap vtable scan took about 17 minutes on a 14.6 GB process. Future scans should walk
  the hero record chain instead of scanning.
