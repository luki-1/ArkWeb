> **Set up ArkWeb with Claude. No technical knowledge needed.** You don't build or configure anything
> yourself: Claude does it on your PC and explains each step. You need both games on Steam, a controller, and a
> PC that can run both games at once.
>
> 1. Download the Claude desktop app from [claude.ai/download](https://claude.ai/download), install it and sign in.
>    (The part of Claude that can work on your PC, Claude Code, comes with Claude's paid plans.)
> 2. Make an empty folder for the mod, for example a folder called **ArkWeb** in your Downloads.
> 3. In the Claude app, open the **Code** tab, start a new session and choose that folder.
> 4. Copy this message, paste it into Claude and send it:
>
>    ```
>    Download ArkWeb from https://github.com/luki-1/ArkWeb into this folder, read its AGENTS.md, and set it up for me: check my PC and games, build it, install it, then guide me through my first session.
>    ```
>
> 5. Claude asks before it does anything on your PC. Allow its requests, and do what it asks you to do when it
>    asks. It will:
>    - check that both games are installed and that Spider-Man is version 4.0630, the only version ArkWeb works with;
>    - install the free tools it needs to build the mod;
>    - build the mod, ask you to close both games, install it, and back up your Arkham Knight save;
>    - tell you which game settings to change;
>    - walk you through your first session: what to load, where to stand, when to start moving, and the controls.
>
> To play again later, open the Claude app, start a new session in the **Code** tab with the same **ArkWeb** folder,
> and say **"Let's play ArkWeb"**. While you play, just tell Claude what's wrong, like "I'm stuck in a building",
> "Spider-Man looks dark" or "I can see Batman". To remove the mod, say **"Uninstall ArkWeb"**.
>
> Other AI agents that can work on your PC (Codex, Cursor and the like) can follow the same guide.

> **Spider-Man stuck inside a building?** Just tell your AI agent to teleport you 200 m into the air.
>
> *For the agent:* write the line `hero tp 0 200 0` to `logs\sm_cmd.txt` (meters relative to Spider-Man; the second number is up). The guest DLL runs it within half a second and logs `teleport ... held` to `logs\sm_guest.log`.

# ArkWeb

https://www.youtube.com/watch?v=O6Mkm_NGCX8

Spider-Man's web swinging in Batman: Arkham Knight's Gotham.

ArkWeb runs **Marvel's Spider-Man Remastered** alongside **Batman: Arkham Knight**. You play Spider-Man's
own movement (swinging, wall running, wall crawling, point launches) with a controller while looking at
Arkham Knight. Gotham's collision is streamed into Spider-Man's physics world, so he swings off and runs on
Gotham's buildings. His own rendered frame is cut out and drawn into Arkham's view in Batman's place.

The architecture follows [SkyCraft](https://github.com/chasmlol/SkyCraft): a hidden guest game drives the
moveset, and the host game shows it.

**Status: experimental.** It works end to end but is a research project with rough edges. See the
known issues below.

## How it works

| Part | What it does |
|---|---|
| `src/sm_guest/` → `winmm.dll` in Spider-Man | Publishes the hero and camera, plays a virtual controller, fakes window focus, and builds Gotham's collision (Havok compressed meshes) about 2 km above New York so New York's own ledges are out of reach. It also patches the hero's transition manager for zip-to-point and perching, and captures Spider-Man's frame (D3D12) for the host. |
| `src/ak_host/` → `dinput8.dll` in Arkham Knight | Drives Batman as a hidden puppet at Spider-Man's position, mimics Spider-Man's camera, and draws the captured frame (D3D11). It also scans Gotham with the game's own traces and exports its PhysX statics and grapple points. |
| `protocol/`, `src/common/` | Shared-memory link (`Local\ArkWeb_v1`): seqlock state slots, rings and coordinate conversion (UE3 Z-up cm ↔ Spider-Man Y-up m) |
| `tools/gotham_stream.py` | The streamer. It follows the hero, asks Arkham for tile scans (50 m tiles) and PhysX exports, builds collision tiles and swing hints, and sends them to Spider-Man. |
| `tools/` (others) | Reverse-engineering helpers (PE, RTTI, strings, disassembly, xrefs), offline fakes of either game, debugging viewers |

Zip to point uses Arkham's grapple points as targets (L2 + R2, with a marker drawn in Arkham's view). With
nothing pressed, Spider-Man perches on the point. A jumps or launches off; B drops.

### Combat

When Arkham says Batman is fighting (its own `IsInCombat` check), the mod hands the fight to Arkham Knight
automatically, and hands it back two seconds after the fight ends:

- Arkham plays Batman with its own freeflow combat, controller and combat camera. Batman stays hidden.
- Spider-Man is pinned to Batman's position and takes Batman's pose every frame. His joints are turned to match
  Batman's skeleton: hips, spine, shoulders, arms, legs and feet, scaled to his proportions.
- Spider-Man's camera copies Arkham's, so the captured Spider-Man lines up with Arkham's picture.

Spider-Man's joint hierarchy isn't stored anywhere the mod could find, so it was worked out from recorded motion
(`tools/sm_pose_sampler.py`, `tools/sm_skel_probe.py`, `tools/ak_skel_probe.py`). `tools/retarget_v2.py` is the
same pose transfer in Python, for checking it offline.

## Requirements

- Windows 10 x64 and a PC that can run both games at once
- **Marvel's Spider-Man Remastered v4.0630** (Steam). Everything is bound to that build's addresses; other
  builds refuse to hook.
- **Batman: Arkham Knight** (Steam)
- An Xbox-style controller
- Visual Studio 2019 Build Tools (x64 C++) and the Windows 10 SDK (for `fxc`)
- Python 3.12 with `numpy`, `scipy`, `numba`, `pefile` and `capstone` (for the streamer and tools)

## Build and install

1. Edit the game paths in `install.bat` and `uninstall.bat`.
2. Run `build.bat`. It builds both DLLs and the link test into `bin\`, at low priority on one core.
3. Close both games, then run `install.bat`. `uninstall.bat` removes the DLLs again.

`arkweb.ini` next to either DLL can override `[ArkWeb] LogDir=` and a few switches (see `PHASE1.md`).
Logs go to `logs\`.

>Pro Tip: Just tell your AI agent to do it for you
>
>*For the agent:* [AGENTS.md](AGENTS.md) walks you through checking the setup, building, installing, and guiding the player.

## Playing

1. Start both games and load into the open world in each. Keep Arkham Knight in front.
2. Stand still with Spider-Man, then start the streamer: `cd tools` and `python gotham_stream.py`.
   It waits for both games, builds the area around you and lifts Spider-Man onto Gotham.
3. Play with the controller. If you restart the streamer while still in game, use `--resume`.

**Your Arkham Knight save follows Batman.** Arkham autosaves while Batman is the puppet, so don't leave
Spider-Man under the map. The host refuses to move Batman far below the streets, and `tp x y z` (feet, UU)
in `logs\ak_cmd.txt` moves Batman back to a safe spot.

### Live commands

Write one command per line to `logs\sm_cmd.txt` (Spider-Man) or `logs\ak_cmd.txt` (Arkham Knight). Results
go to `logs\sm_guest.log` and `logs\ak_host.log`.

| Game | Examples |
|---|---|
| Spider-Man | `hero tp 0 200 0`, `zip status`, `zip perch on\|off`, `gotham stream status`, `gotham surface <hex>`, `cap status` |
| Arkham Knight | `tp x y z`, `batman hide\|show\|auto`, `overlay on\|off`, `overlay gamma <g>`, `shot`, `cam mimic on\|off`, `px info` |

## Known issues

- Arkham's building collision is often hollow, and Gotham comes from vertical scans plus PhysX. Thin spires
  and very narrow buildings can still be swung through.
- Swinging very fast into a district Arkham hasn't loaded yet can leave gaps for a few seconds, until
  it loads and the tile is rescanned.
- A zip whose path is blocked is cut short and Spider-Man drops (there's no line-of-sight check yet).
- Spider-Man can look dark in places (his lighting comes from New York's time of day).
- In combat, hands and wrists aren't copied (no fists), and the camera cuts at the start and end of a fight.
- The combat pose transfer is tied to the suit it was recorded with. A suit with a different skeleton is detected
  (bone lengths are checked) and the mirror stays off.
- Do **not** run `bin\link_test.exe` while the games are running. It opens the live shared memory.

## Repository layout

| Path | Contents |
|---|---|
| `src/`, `protocol/` | The two DLLs and the shared link |
| `tools/` | The streamer and reverse-engineering scripts (`tools/README.md`) |
| `tests/` | Offline link test |
| `recon/PHASE0*.md`, `PHASE1.md`, `PHASE2.md` | Findings and phase notes, with addresses |
| `recon_dll/` | The passive recon probes used at the start |

Not included: `logs/`, build output, and the raw dumps of the game executables (strings, RTTI, the UE3
SDK, disassembly). The scripts in `tools/` regenerate those from your own copies of the games.

## Disclaimer

An unofficial fan project, not affiliated with Insomniac Games, Nixxes, Sony Interactive Entertainment,
Rocksteady Studios or Warner Bros. Games. It contains no game files; you need your own copies of both
games.
