# ArkWeb Phase 0: static recon findings

2026-10-01. Static analysis only; neither game was run. All addresses are RVAs (add the module base at
runtime) for **Spider-Man.exe v4.0630.0.0** (122 MB) and **BatmanAK.exe** (54.6 MB, Oct 2026 Steam build).
Tools: `../tools/*.py` (pefile + capstone + numpy, idle priority, one core).

## Verdict: go

The big unknown was whether Spider-Man's swing needs authored New York data. It doesn't. Swing anchors,
wall runs and ground movement all come from **live collision queries**, and every one of them goes
through a single query layer that ends in three Havok functions. That layer is the hook point
(SkyCraft's `BlockCollisionsMixin` equivalent), so Spider-Man's own traversal code can run against
Gotham.

---

## Spider-Man Remastered (guest)

**Engine facts**
- Insomniac engine, Nixxes port. MSVC RTTI is intact: 16,242 classes and 16,398 vtables
  (`sm_rtti.json`, `sm_classes.txt`).
- Physics is Havok **hknp 2017.1.0-r1**. Units are **meters** (swing config text: "Radius of wobble
  around center line in meters").
- No packer. Normal PE sections.

**Traversal state machine** (RTTI classes, `*Local` = locally driven player)
- Swinging: `Hero::HeroStateSwing` / `HeroStateSwingLocal`, `HeroStateSwingJump(Local)`,
  `HeroStateSwingIntroJump(Local)`, `HeroStatePoleSwing`
- Web zip: `HeroStateWebZip`, `HeroStateZipToPointLaunch`, `HeroStateZipToWall`, `HeroStateZipToLedge`
- Wall run and crawl: `HeroStateWallRun*` (about 20 states), `HeroStateWallCrawl*`
- Perching: `HeroStatePerch*`, `HeroStateFlyToPerch`
- Ledges and vaults: `HeroStateLedgePathBase` and its derived states
- Hero object: `Hero::HeroLocal` (24 vmethods), `Hero::HeroMoverManager`, `Hero::HeroCameraManager`
  (38 vmethods), `Hero::HeroIKManager`

**How swing anchors are found.** They come from collision tests:
- `Hero::SwingCollider::IssueLookAheadTests` @ `b8fe50`, `IssueSwingLineTest` @ `b90350`/`b91bb0`,
  `IssueWallPlantTests` @ `b90460`
- `SwingPointHunter::IssuePotentialValidationTests` @ `874520`
- `HeroTraversalUtil::IssueDirect*Test`, `HeroTraversalTargeting::IssueWebZipAttachCollision`,
  `WallHunter`, `WallContourProbe`, `WebLineEndPointProbe`
- When no anchor is found, the game falls back to "swing fail" (`OnSwingFailed`,
  `SwingFailZipData`, "Option to turn off down-zip on fail-to-swing"). There is no hidden anchor
  table.
- Authored helpers exist but are optional tuning: `SwingHintVolume`, `SwingAttachClue`,
  `HeroSwingSetupVolume`, `SwingPushVolume`, `HeroTraversalFloorHint`, `Hero::TraversalMarkupTrackingSystem`,
  `CustomPointLaunch`. Phase 3 can generate the useful ones (point-launch, perch) from Arkham Knight's
  grapple points.
- Ground movement uses the same layer (`MoveProcessorGroundStandard::IssueBucketedQueries`). There is
  no Havok character proxy to fight.

**Query layer (the hook point)**
```
gameplay  ── Physics::CollRequest ──▶ NQS submit (1826490 ray / 1826600 sphere, ...)
          ── Physics::NQS::N{RayCast,SweptSphere,SweptCapsule,ShapeCast,ClosestPoints}Query
             (each embeds an ighkCollisionQueryCommands::* object at +0xf0 / +0x100 / +0x130)
          ── ighkCollisionQueryCommands::CastRayVsWorld::Execute            @ 1818070
             ::CastShapeVsWorldWithStartCollector::Execute                  @ 1818170
             ::ClosestPointsShapeVsWorld::Execute                           @ 1818280
          ── hknpWorld::castRay        @ 2e67010  (profiler tag "WorldCastRay")   6 callers
             hknpWorld::castShape      @ 2e670f0                                 11 callers
             hknpWorld::getClosestPoints @ 2e671f0                                4 callers
             (each → world->m_collisionQueryDispatcher [+0x4a0], vtable slot +0xa0)
global hknpWorld* (the game world): [78939e8]
```
- The game-side (non-Havok-internal) callers are `1817190`/`1817730` (immediate rays), `1816450`
  (immediate shape), `1816c40` (immediate closest points), and the three async `Execute`s above.
  Hooking the three `hknpWorld::*` entry points, filtered on `world == [78939e8]`, covers async and
  immediate queries alike.
- Results carry Havok body ids and shape keys. Surface type comes from shape tags
  (`Physics::IgShapeTagCodec`, vtable `3d0ae20`). New York has authored invisible collision such as
  `hero_coll_wall_run_4x4.model`, so wall-runnability is a surface tag. Gotham triangles need tags
  that say "wall-runnable".

**Two ways to put Gotham into the queries** (decided at runtime in Phase 2)
1. **Recommended: a filtered world.** Add Gotham as static hknp bodies on a reserved collision layer
   of the game world. In the three hooks, replace the query filter so hero queries see only that layer.
   Body ids, shape tags and materials then stay valid for everything downstream.
   - Needs: hknp body/shape creation found by signature (`hknpCompressedMeshShape` construction,
     `hknpWorld::createBody`). Havok profiler strings make these findable.
2. **Fallback: answer the queries ourselves.** Answer from our own BVH (SkyCraft's `TriCollider`) and
   fabricate `hknpCollisionResult`s. This is fragile if downstream code dereferences body ids.

**Input** (Nixxes `nx` layer)
- Gamepads: `nx::Input::GamepadXInput` (XInput loaded dynamically, `XInputGetState` by name),
  `GamepadSce` (DualSense through HID/libScePad), `GamePadSteam`.
- Keyboard and mouse: `NxKeyboardDevice` / `NxMouseDevice` on `RegisterRawInputDevices` +
  `GetRawInputData`.
- Plan for v1: MinHook `XInputGetState` inside the loaded xinput DLL and feed a virtual pad from the
  protocol. v2: inject `WM_INPUT` / hook `GetRawInputData` for keyboard and mouse.

**Window and focus**
- `nx::NxGameWindowImpl` / `NxAppImpl`. The game imports `GetForegroundWindow`, `GetFocus` and
  `ClipCursor`.
- Fake focus by hooking `GetForegroundWindow`/`GetFocus` and swallowing `WM_ACTIVATEAPP`/`WM_KILLFOCUS`
  in a WndProc subclass (SkyCraft's `WindowMixin`).

**Loader**
- `WINMM.dll` is a static import and not a KnownDLL, so a `winmm.dll` proxy next to the exe works.

**Assets (for Phase 5)**
- `asset_archive/toc` is magic `0x77AF12AF` + size + zlib. Decompressed it is a `DAT1` blob
  (21.7 MB).
- `dag` is also zlib → `DAT1` (53.8 MB), with readable asset names: 93k `.model`, 131k `.animclip`.
- Archives `g00sNNN` are `DSAR` (Nixxes chunked compressed archives).
- 133 suit models are named `hero_spiderman_*.model`, plus the `hero_peterparker_*` heads.
- Extraction is feasible on the user's machine (no assets get redistributed).

## Arkham Knight (host)

**Engine facts**
- UE3 (Rocksteady fork), D3D11, **PhysX 3.3.1** (`PhysX3_x64.dll` 3.3.1.0), APEX.
- No RTTI (50 classes, all CRT/STL). No packer.

**Native function table: our "symbols".** There are 6,903 `UClassexecFunction` entries, all resolved
to addresses (`ak_natives.json`). Selected entries:

| Use | Native | RVA |
|---|---|---|
| Puppet move | `AActorexecSetLocation` / `SetRotation` / `SetPhysics` / `SetCollision` | `b36bf0` / `b36c90` / `9ffba0` / `9b9d70` |
| Puppet hide | `AActorexecSetHidden` (same function as `ARPawnCharacterexecSetHidden`) | `4a1f80` |
| Puppet move (alternative) | `ARPawnexecSetLocationIgnoringCollision`, `ARPawnexecTeleportPhysics` | `472110`, `b445e0` |
| Camera | `AR3rdPersonCameraexecUpdateViewTarget` / `DoUpdateCameraNative` / `ProcessViewRotation` | `512940` / `512810` / `512650` |
| Batman's own swing (reference only) | `ARCwGrappleGunBaseexecCalculateSwingPath`, `CheckAerialSwing`, ... | `56c580`, `48fe30` |

Class counts: `ARPlayerController` (347 natives), `ARPawnCharacter` (144), `ARPawnPlayer` (79).

**Grapple data** (for Spider-Man perch and point-launch markers): `ARGrapplePoint`,
`ARGrapplePointCollection`, `ARHidePointLedgePerch` (gargoyles), `ARShimmyEdgeMarker`,
`ARCeilingClimbVolume::FlagGrapplePoints`.

**UE3 core**
- `FName::StaticInit` @ `f2e410`. The GNames candidate is the TArray at `[3a09f30]` (data) /
  `[3a09f38]`, to be verified at runtime.
- `StaticFindObject` @ `f3b7f0`.
- GObjObjects is not pinned yet. The runtime SDK dumper will find it (UObject index walk).
- `FFrame` uses **4-byte packed offsets** (`Node` at +0x0C, `Object` at +0x14, `Code` at +0x1C in
  `execGetFuncName` @ `f78c40`). That means Rocksteady builds with `pragma pack(4)`, and the SDK
  generator must match it.

**PhysX scene access** (collision export, Phase 2)
- `PhysX3_x64.dll` exports `PxGetPhysics`, so the path is
  `PxPhysics::getScenes → PxScene::getActors(eRIGID_STATIC) → PxShape`.
- The geometry helpers `PxShapeGeometryPropertyHelper::getGeometry(...)` are exported too.
- Vtable slots need 3.3 headers (NVIDIA GameWorks, needs an account) or disassembly checks against
  3.4's public headers.
- `URAsyncPhysXSceneInterface` exists, so AK runs an async scene. Read it on the game thread at a
  sync point.

**Input and loader**
- Imports `DINPUT8.dll`, `XINPUT9_1_0.dll` and `libScePad`.
- A `dinput8.dll` proxy works because DINPUT8 is not a KnownDLL on this machine.

## Still open (needs one game running, one at a time)

1. **AK:** confirm GNames, find GObjObjects, dump the SDK (class/struct layouts, the `ProcessEvent`
   slot), and measure units: Batman's collision cylinder height in UU versus PhysX scale.
2. **SM:** confirm that hero traversal queries hit the three `hknpWorld` entry points (counts per
   frame while swinging), and that `[78939e8]` is the main world. Find the hero transform and the
   final skinning matrices from `Hero::HeroLocal`.
3. **SM:** the shape-tag layout for wall-runnable surfaces, read from hits on New York buildings.

Both can be done with **passive, log-only recon DLLs**: no rendering changes, no gameplay changes, and
a few seconds in-game.
