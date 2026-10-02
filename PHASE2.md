# ArkWeb Phase 2: Gotham collision inside Spider-Man

## 2a. Arkham Knight geometry export: working (2026-10-01)

The AK host has dev commands (`logs/ak_cmd.txt`, see `src/ak_host/devcmd.h`). They run on the game
thread, or on the worker while AK is paused for lost focus.

**PhysX 3.3.1 access without the SDK** (`src/ak_host/px.h`)
- Vtable slots were recovered from PhysX's reflection metadata (`tools/px_meta.py`) and checked in
  disassembly:
  - PxPhysics: getNbScenes 23, getScenes 24
  - PxScene: getNbActors 17, getActors 18. **The actor-type flags go by pointer.** Passing the
    value crashed the game once.
  - PxRigidActor: getGlobalPose 20 (hidden return), getNbShapes 25, getShapes 26
  - PxShape: getGeometryType 5, getLocalPose 17 (hidden return)
- Geometry comes from the exported `PxShapeGeometryPropertyHelper::getGeometry` overloads
  (`this` unused, shape in rdx, output struct in r8). PxConvexMeshGeometry is 0x28 bytes with the
  mesh pointer at +0x20.
- **Convex hull data** is in the mesh object at +0x20:

  | Offset | Field |
  |---|---|
  | +0x20 | AABB (6 floats) |
  | +0x38 | centre of mass |
  | +0x44 | u16 nbEdges |
  | +0x46 | u8 nbVerts |
  | +0x47 | u8 nbPolys |
  | +0x48 | polygons pointer (20 bytes each); the vertices follow the polygons |

  The layout was confirmed on 3 meshes (box hull: 12/8/6), and all vertices lie inside the AABB.
- Triangle meshes: layout detected per mesh (only 2 exist).

**Gotham's static collision**
- 39,092 static actors with 73,711 shapes: **39,000 convex**, **34,709 boxes**, 2 triangle meshes.
- 5 scenes; scene 0 holds 39k of the static actors.
- **Scale: 0.02 PhysX units per UU** (1 PhysX unit = 50 UU = 0.5 m).

**First export** (`logs/gotham_150m.obj`, rendered to `gotham_150m_view.png` / `_top.png`)
- 42,237 shapes within 150 m of Batman. Offline that's 594k triangles once the convex hulls are
  rebuilt (scipy).
- Only 3 of 38,997 convexes were unreadable.
- Recognisable Gotham: buildings, smokestacks, cranes, bridges, rooftops.

## 2b. Next: Spider-Man side
- Stream shapes host -> guest over the collision ring as convex point sets and boxes (guest space)
  and instance them near the hero.
- Build Havok shapes in Spider-Man (`hknpConvexPolytopeShape` from points, or a compressed mesh
  from hkGeometry), add them as static bodies, and filter the hero's queries to Gotham only.

## 2b. Spider-Man runtime collision: static RE so far (Spider-Man.exe 4.0630)

The template is **`TerrainPhysics::CreateTile`** @ `1998230`, the game's own path for adding static
collision at runtime (`recon/sm_terrain_createtile.asm`).

**Steps CreateTile follows**
1. **Shape.** A heightfield in CreateTile. For Gotham:
   - **Box:** `hknpConvexShape::createFromHalfExtents` @ `2e44130` (rcx = `const hkVector4* halfExtents`,
     xmm1 = radius, r8 = `const hkTransform*` or null). It allocates 0x1d0 bytes from the Havok heap
     and constructs an hknpBoxShape.
   - **Convex:** `hknpConvexShape::createFromVertices` @ `2e44410` (rcx = `hkStridedVertices*`
     {ptr, int count, int stride 16}, xmm1 = radius, r8 = `BuildConfig*`).
     - BuildConfig (≥0x30 bytes) gets its defaults from the ctor @ `2e43670`. Havok's own caller
       then sets byte +0x11 = 0 and byte +0x2d = 0. +0x20 is a transform pointer (null = none).
     - Havok assert: "Couldn't create and simplify the hull from provided vertices".
2. **hknpPhysicsSystemData**, built by hand at block+0x80 (refcounted object):

   | Offset | Field |
   |---|---|
   | +0x00 | vtable `exe+3d022d8` |
   | +0x08 | 0 |
   | +0x10 | qword 0x1ffff, then u16 0xffff |
   | +0x18 | hkArray materials (0x70 each) |
   | +0x28 | motionProperties (0x70) |
   | +0x38 | bodyCinfos (0xB0) |
   | +0x48 | constraints |
   | +0x58 | referenced objects (8) |
   | +0x68 | name |
   | +0x70 | byte 1 |

   - hkArray = {ptr, int size, int capacityAndFlags}; 0x80000000 = don't deallocate.
   - Arrays grow with `2aee6e0(allocator=2adc520(), &array, elemSize)`.
3. **Material.** Default ctor `2e25800`; name `2aeecd0`; +0x28 = 0, +0x38 byte 2, +0x39 byte 2,
   +0x3C float = [exe+382e120]. Copied into the array with `2e257a0`.
4. **Motion properties.** 0x70 bytes copied from the preset at `[[[exe+609a570]+0x18]+0xa20]+0x30`.
5. **Body cinfo (0xB0).** Default ctor `2e24b30`, then:

   | Offset | Field |
   |---|---|
   | +0x00 | shape* |
   | +0x10 | collisionFilterInfo = **0x243f** (terrain) |
   | +0x14 | u16 material index 0 |
   | +0x18 | hkStringPtr name |
   | +0x20 | userData |
   | +0x28 | byte 0 |
   | +0x30 | position (vec4) |
   | +0x40 | orientation (quat xyzw) |
   | +0x70 | float [exe+382f0e4] |
   | +0x78, +0x90 | refptrs |
   | +0xA0 | int -1 |

6. Shape added to the data's referenced objects (+0x58, addReference `2aee150`); name at +0x68
   from [exe+6208438].
7. **Add to the world.**
   - Construct: `hknpPhysicsSystem::hknpPhysicsSystem(this = 0x50-byte block, world = [[exe+609a570]+0x18], data, transform = &[exe+6b14af0] (identity), flags 3)` @ `2e400c0`
   - Add: `2e410d0(system, 1, 0)`

**Still to resolve**
- That `[[exe+609a570]+0x18]` is the same world as the query hook's `[exe+78939e8]`.
- **Query filter.** Every hero query carries `hknpCollisionFilter*` at query+0x00. To hide New York,
  substitute a clone of that filter whose body-level `isCollisionEnabled` only accepts Gotham bodies.
  This needs the hknpCollisionFilter vtable slot (8 methods; group filter vtable `exe+5287660`).

## 2b implementation (built 2026-10-01, not yet run)

- `src/sm_guest/havok.h`: addresses and layouts above. `src/sm_guest/gotham.h`: hull loading,
  shape and batch building, the shape-pointer set, and the slot-6 query-filter detour with
  hknpBody shape-offset self-calibration.
- **Hooks:**
  - frame update `exe+16244c0`: main thread; Pump builds 64 shapes per frame and runs dev commands
  - `hknpWorld::castRay`: patches the query filter's vtable once
- **Commands** (`logs/sm_cmd.txt`):
  - `gotham load <file> [limit] [yoff]`: the file's origin goes to the hero's feet
  - `gotham filter on|off`
  - `gotham status`
  - `gotham perframe N`
  - `mem`, `ptr`
- `tools/obj_to_gotham.py`: converts an AK export into a hull file (guest m, Y up, relative to
  Batman's feet). Near-flat hulls are thickened by 10 cm because Havok's hull builder rejects
  flat sets.
- `logs/gotham_150m.bin`: 42,237 hulls from the first export (3,181 thickened).

**First test plan**
1. `gotham status`
2. `gotham load gotham_150m.bin 50`: 50 nearest hulls, a single batch
3. Check the log: system added, body count, no crash
4. `gotham filter on`: the hero should stand on the Gotham roof only
5. Scale up (2000, then everything), watching for a body-capacity limit
