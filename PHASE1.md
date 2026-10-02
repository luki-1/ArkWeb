# ArkWeb Phase 1: link

Goal: both games handshake over shared memory. The coordinate mapping is unit-tested. Each side can
be developed and tested with only its own game running.

## What exists

| Path | What |
|---|---|
| `protocol/arkweb_protocol.h` | Shared-memory layout `Local\ArkWeb_v1`: header and heartbeats, HostState, PadState (XInput-shaped), GuestState, SPSC rings |
| `src/common/link.h` | Mapping, seqlock read/write, SPSC byte ring |
| `src/common/coords.h` | AK (UE3, Z-up, LH, cm) <-> Spider-Man (Y-up, m) |
| `src/common/util.h` | Logging (`ArkWeb/logs`), safe reads, inline / IAT / vtable-capture hooks |
| `src/ak_host/` | **dinput8.dll** for Arkham Knight. Hooks `UObject::ProcessEvent` and, on the player controller's `PlayerTick`, publishes Batman and drives him from the guest's hero (`SetPhysics(None)`, `RPawn.SetLocationIgnoringCollision`, `SetRotation`). Lets go with `PHYS_Falling` when the guest disappears. |
| `src/sm_guest/` | **winmm.dll** for Spider-Man. Captures `Hero::HeroLocal` through its vtable and publishes the hero transform every ~4 ms. Plays the link's virtual pad as XInput controller 0 (through `GetProcAddress`). Fakes focus while a host is alive (`GetForegroundWindow`/`GetFocus`, swallowed deactivation messages, `ClipCursor`/`SetCursorPos` ignored while really in the background). |
| `tests/link_test.cpp` | Coordinate round trips and handedness, seqlock under contention, ring integrity, `--layout` JSON |
| `tools/arkweb_proto.py` | Python mirror; `--check` verifies it against the C++ layout |
| `tools/fake_spidey.py` | Guest stand-in: replays the recorded swing, anchored at Batman |
| `tools/fake_arkham.py` | Host stand-in: heartbeat plus scripted pad (`monitor`, `handedness`, `swing`), records the guest |
| `build.bat`, `install.bat`, `uninstall.bat` | Build (low priority, one core), copy into both games, remove |

`arkweb.ini` next to either DLL can override `[ArkWeb] LogDir=`, `DrivePuppet=` (AK, default 1),
`TickEvent=` (AK, default `PlayerTick`) and `ForceFocus=` (SM, default 0).

## Offline results (2026-10-01)

- `link_test`: all passed. Seqlock: 0 torn reads under a hot writer. Ring: 200k random records
  through a 4 KB ring, 0 bad.
- `arkweb_proto.py --check`: layout OK.
- `fake_arkham` <-> `fake_spidey` end to end: linked, 59 Hz guest publish, the host recorded 698 samples in 12 s.

## In-game tests (one game at a time)

1. **AK + fake_spidey.** Batman should be carried along the recorded swing path through Gotham,
   then dropped when the replay ends. Check `logs/ak_host.log` (resolve line, ProcessEvent census,
   "driving Batman") and `logs/fake_spidey.log` (Batman's distance from the hero should stay small).
2. **SM + fake_arkham.**
   - `monitor`: the hero is published and focus is faked, so the game keeps running when you
     alt-tab.
   - `handedness`: settles `coords::kGuestRightHanded`.
   - `swing`: the virtual pad makes Spider-Man run and swing.

## In-game results (2026-10-01)

### Test 1: Arkham Knight + fake_spidey (pass)

- The ProcessEvent hook resolved everything. `PlayerTick` ticks the host on the game thread.
- Batman was carried along the full 235 s replay, **2.2 m from the hero on average (3.8 m max)**,
  and the camera followed.
- Expected artifacts:
  - falling pose (physics is off; Batman gets hidden in Phase 4 and replaced in Phase 5)
  - clipping through buildings (the path is a New York path; Phase 2 makes Spider-Man collide with
    Gotham)
- Fixed afterwards: a torn read of the guest state counted as "guest gone", which caused 6 brief
  drop-and-regrab blips. The host now keeps the last good state until the heartbeat times out.

### Test 2: Spider-Man + fake_arkham (pass, after four fixes)

1. **Windows.Gaming.Input.** The game prefers WGI, and once WGI found the user's Xbox controller it
   logged "XInput disabled". The guest now IAT-hooks the registry reads (`RegQueryValueExW/A`,
   `RegGetValueA`) and reports `EnableWindowsGamingInput = 0` for that run. The user's registry is
   never written (`VirtualPad=1`, the default).
2. **Keyboard path.** XInput is only polled for slots with a real Xbox device, so the virtual pad
   also drives the keyboard bindings, read from the game's own settings.
   - The guest posts `WM_INPUT` with tagged handles and answers its own `GetRawInputData`
     (IAT hook).
3. **Stale window.** The game destroys its first window and creates a new one. The guest now finds
   the window by class `GameNxApp` and re-finds it whenever the handle dies.
4. **App-active flag.** The window procedure only reads raw input while
   `NxGameWindowImpl::IsActive()` holds: flags +0x18 and +0x19 set, +0x1A and +0x1B clear, on the
   object at `[[exe+7b11770]+0xa0]`. +0x19 drops when focus is lost.
   - While focus is faked, the guest holds +0x18 and +0x19 at 1 and also posts activation messages.
   - This was validated by writing the flag from outside first; it is now built into the DLL but
     not yet re-tested from the DLL.

**Results**
- **Handedness:** three forward/right stick runs gave (forward × up) · right = +1.00, +1.00, +0.99.
  **Spider-Man is right-handed**, so `kGuestRightHanded = true` is confirmed.
- **Swing by virtual input:** run, jump and swing from a rooftop covered 428 m in 20 s, between
  street level and 106 m up.
- Once active, both input paths work (XInput virtual pad served, injected keys read).
- **Velocity spikes fixed:** the guest samples every 4 ms but the hero moves once per frame. Velocity
  is now computed only across real position changes.

**Phase 1 is complete.** Still open from it: a quick re-run of `fake_arkham.py handedness` once the
rebuilt guest (built-in active-flag hold) is installed, to confirm that no outside helper is needed.
