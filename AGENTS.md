# ArkWeb: guide for AI agents

This file is for an AI coding agent (Claude Code, Codex, Cursor and the like) helping someone install and play
ArkWeb. `README.md` is the human overview; this file is the procedure. Follow it top to bottom the first time.
Later sessions ("Let's play ArkWeb") can jump to [Playing](#5-playing-session) and [While they play](#6-while-they-play).

ArkWeb runs **Marvel's Spider-Man Remastered** (the hidden *guest*) next to **Batman: Arkham Knight** (the
*host*, the window the user looks at). Spider-Man's own movement is played with a controller, Gotham's
collision is streamed into Spider-Man's physics, and his rendered frame is drawn into Arkham's view in
Batman's place. Three pieces must run together: `winmm.dll` in Spider-Man, `dinput8.dll` in Arkham Knight,
and the Python streamer `tools/gotham_stream.py`.

## 0. Ground rules

- **Assume the user isn't technical and uses Windows only through its normal interface.** Run every command
  yourself. When they must do something, say exactly where to click (which app, which menu, which button); never
  ask them to open a terminal, type a command or edit a file.
- **Ask before anything the user must do.** Before each step that needs them (closing or starting a game,
  loading a save, playing, testing), say what they'll do and roughly how long, then wait for an explicit OK.
  One OK doesn't cover the next step.
- **Their Arkham Knight save follows Batman.** Arkham autosaves while Batman is the puppet. If Spider-Man ends up
  under the map, a save there can spawn Batman under the map for good. Back the save up before the first session
  (step 3), keep Spider-Man above the streets, and rescue him early (see [Rescue](#rescue)).
- **Never run `bin\link_test.exe` while either game runs.** It opens the live shared memory and writes fake
  state into it.
- **Don't restart the streamer while the user plays.** A restart leaves seconds with no streaming, and
  Spider-Man can walk into an unbuilt tile and fall. If you must, restart with `--resume`.
- **Install or uninstall only with both games closed.**
- **No experiments on a running Arkham Knight.** An access violation inside Arkham is a crash, even inside
  `__try`. Stick to the commands in the README.
- **Both games at once is heavy.** Builds run at low priority on one core by design. Don't add load-heavy work
  (big builds, scans, GPU tests) while the user plays.
- When you watch the streamer's output, report errors and milestones only, not every status line.

## Getting the repository

If the user started you in an empty folder and asked you to download ArkWeb, put the repository's files
**directly in that folder**, so that `AGENTS.md` and `CLAUDE.md` sit at its top level and later sessions opened
there find this guide. Use `git clone https://github.com/luki-1/ArkWeb .` if git is installed. Otherwise download
`https://github.com/luki-1/ArkWeb/archive/refs/heads/main.zip`, extract it, and move the contents of its
`ArkWeb-main` folder up into the user's folder.

## 1. Check the user's setup

Do these checks yourself, then tell the user what's missing. Install missing tools yourself (for example with
`winget`) after asking them, rather than sending them to do it.

| Check | How |
|---|---|
| Windows 10 x64 | ArkWeb was built and tested there. Windows 11 is untested. |
| Spider-Man Remastered, Steam, **v4.0630** | `(Get-Item "<SM folder>\Spider-Man.exe").VersionInfo.FileVersion` must be `4.0630.0.0` (Steam app 1817070). Every hook is bound to that build's addresses. With another build the guest can't find what it hooks, and nothing works. |
| Batman: Arkham Knight, Steam | `<library>\steamapps\common\Batman Arkham Knight\Binaries\Win64\BatmanAK.exe` (Steam app 208650) |
| Game folders | Steam library paths are listed in `C:\Program Files (x86)\Steam\steamapps\libraryfolders.vdf`. Spider-Man is `<library>\steamapps\common\Marvel's Spider-Man Remastered\`. |
| Other mods in the way | If `winmm.dll` is already in the Spider-Man folder, or `dinput8.dll` in Arkham's `Binaries\Win64`, it belongs to another mod and the install would overwrite it. Tell the user and back it up before installing. |
| Xbox-style controller | It's read as XInput controller 0. Without a controller, the keyboard fallback is in step 5. |
| Hardware | Both games run at the same time. The author used an RTX 3080 with 32 GB of RAM. |
| C++ build tools | Visual Studio 2019 Build Tools (x64 C++); `build.bat` calls `C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat`. A different VS version or edition needs that path changed (others are untested). `vswhere.exe` in `C:\Program Files (x86)\Microsoft Visual Studio\Installer\` finds installs. |
| Shader compiler | `fxc.exe` from the Windows 10 SDK; `build.bat` expects `C:\Program Files (x86)\Windows Kits\10\bin\10.0.19041.0\x64\fxc.exe`. Point `FXC` at another SDK version if needed. |
| Python | 3.12 with `numpy scipy numba pefile capstone` (`python -m pip install numpy scipy numba pefile capstone`) |

## 2. Build

1. Fix the `vcvars64.bat` and `FXC` paths in `build.bat` if step 1 found them elsewhere.
2. Run `build.bat` from the repository root. It ends with `BUILD OK` and writes:
   - `bin\sm\winmm.dll` (the Spider-Man guest)
   - `bin\ak\dinput8.dll` (the Arkham Knight host)
   - `bin\link_test.exe` (an offline test of the shared-memory link)
3. Optional, **only with both games closed**: run `bin\link_test.exe` to check the link.

## 3. Install

1. Set the two game folders at the top of `install.bat` (`AK` = Arkham's `Binaries\Win64`, `SM` = the
   Spider-Man folder) and the same two paths in `uninstall.bat`. They hold the author's paths.
2. Ask the user to close both games, then run `install.bat`. It copies the DLLs and writes an `arkweb.ini` next
   to each one. Keep those files: they point both games at this repository, where they exchange files with the
   streamer. Existing settings in them are kept. Optional keys for the `[ArkWeb]` section:
   `DrivePuppet=` (AK, default 1), `MimicCamera=` (AK, default 1), `Overlay=` (AK, default 1),
   `TickEvent=` (AK, default `PlayerTick`), `VirtualPad=` (SM, default 1), `ForceFocus=` (SM, default 0).
3. Back up the Arkham Knight save. Steam keeps it in `C:\Program Files (x86)\Steam\userdata\<id>\208650\remote\`
   (`BAK1Save*.sgd`). Copy that folder somewhere outside Steam.

To remove ArkWeb, close both games and run `uninstall.bat`. The `arkweb.ini` files can stay or go.

## 4. Game settings to tell the user

These only need doing once.

- **Spider-Man**: play it **windowed**, at a moderate size (only Spider-Man himself is shown, cut out of this
  window; a bigger window looks sharper but costs GPU time). Turn **off** the HUD, motion blur, depth of field,
  film grain, HDR (use SDR) and frame generation. Any of these leaks into the cut-out or breaks the depth test
  that finds him.
- **Arkham Knight**: windowed or borderless makes switching windows painless. Nothing else is required.
- Both games play sound. Lowering Spider-Man in the Windows volume mixer is up to the user.
- Optional: Spider-Man's lighting follows New York's time of day. After the story, the research station lets the
  user choose night, which matches Gotham better.

## 5. Playing session

Ask before each numbered step that needs the user.

1. **Start both games and load into the open world in each.** Free roam, no mission, no cutscene. In Arkham,
   Batman should stand **on foot on a street** (not in the Batmobile, not on a rooftop edge): his feet become
   Spider-Man's starting spot. In Spider-Man, stand still on the ground. Leave **Arkham Knight in front**.
2. **Start the streamer** in a background shell whose output you can read:

   ```bash
   cd tools && python gotham_stream.py
   ```

   It waits for both games, prints `origin: Batman's feet ...`, compiles its hull builder (a few seconds), builds
   the start area, then prints `start area built in Spider-Man: jumping up onto Gotham` and
   `Spider-Man stands on Gotham`. Gotham sits 2 km above New York (`--sky`), so New York's own ledges are out
   of reach. If it prints `the jump didn't hold`, it places Gotham at his feet instead, which also works. From
   then on, Batman follows Spider-Man and Spider-Man is drawn in his place.
   **Tell the user not to move Spider-Man until you say he's on Gotham.**
3. **Tell the user they can play**, and give them the controls:

   | Input | Does |
   |---|---|
   | Left stick / right stick | Move / Spider-Man's camera (Arkham's view copies it) |
   | RT (hold) | Web swing; hold while running at a wall to run up it |
   | A | Jump; release a swing with A for a swing jump |
   | B | Dodge |
   | LT + RT | Zip to the grapple point under the ring marker in Arkham's view |
   | After a zip | He perches on the point. A jumps or launches off; B drops. |
   | Fights | Automatic. When Arkham decides Batman is fighting, Arkham's own combat takes over (X strike, Y counter, and so on), with Spider-Man in Batman's place copying his moves. Control returns 2 s after the fight. |

   Without a controller (Arkham in front): WASD = left stick, Space = A, Left Shift = RT, Left Ctrl = B.

4. **End of session.** Before quitting, have the user bring Spider-Man down to a street, so Batman (the save) is
   left somewhere normal. Stop the streamer (Ctrl+C or kill the process), then close the games in any order.
   When Spider-Man closes, Arkham takes Batman back wherever he is.

## 6. While they play

The README explains how to send live commands to either game (*Live commands*, and the note at its top). The
streamer prints the hero's position and speed and the tiles it holds every few seconds; positions are Arkham
units (UU, centimeters, Z up).

### Rescue

| Situation | Do |
|---|---|
| Spider-Man is stuck inside a building, or the user asks to be teleported up | Spider-Man: `hero tp 0 200 0` (meters relative to him; the second number is up) |
| He fell under Gotham | The guest puts him back by itself once he is 150 m below Gotham. If it doesn't, `hero tp 0 <meters> 0`; work out the height from the streamer's position line first. |
| Batman is under the map or stuck after Spider-Man closed | Arkham: `tp -65886 -57301 -234` (a known-good street, Arkham units) |
| Batman stops following while Spider-Man is low | Batman refuses to follow below the streets (this protects the save). Rescue Spider-Man and Batman follows again. |

### Troubleshooting

| Symptom | Likely cause and fix |
|---|---|
| Streamer: `ArkWeb mapping has an incompatible protocol` | The installed DLLs are from another build than `tools/arkweb_proto.py`. Rebuild, close both games, reinstall. |
| Streamer waits forever for both games | A game isn't in the open world yet, Spider-Man isn't standing still, or a DLL isn't installed: check that `dinput8.dll` and `arkweb.ini` are in Arkham's `Binaries\Win64` and `winmm.dll` and `arkweb.ini` in the Spider-Man folder. |
| Spider-Man doesn't react to the controller | Arkham must be in front; the controller is read through Arkham. |
| Spider-Man isn't drawn in Arkham | He is drawn only while he is on Gotham and linked (after `Spider-Man stands on Gotham`). Arkham: `overlay on` if it was switched off. |
| Spider-Man looks dark | Arkham: `overlay gamma 0.7` (under 1 is brighter; his lighting is New York's). |
| Batman visible next to Spider-Man | Arkham: `batman show`, then `batman auto`. |
| Spider-Man falls through a road or roof | The tile was scanned before Arkham loaded that district. The streamer rescans such tiles when he comes close. Rescue him with `hero tp`, and wait a few seconds before swinging back. |
| Stutter that grows over time | Watch the streamer's tile counts. Don't restart the streamer during play. |
| Spider-Man crashed | Its crash dumps are in `Documents\Marvel's Spider-Man Remastered\`; `python tools\minidump.py <dump>` reads them. |

## Known limits to set expectations

Tell the user about the README's *Known issues* before the first session, so a glitch isn't mistaken for a
broken install. In short: hollow Arkham buildings and thin spires can be swung through, fast swings into
unloaded districts can leave gaps for a few seconds, blocked zips drop him, fights have no fists, and a suit
with a different skeleton turns the combat pose copy off.

## Where to look in the code

| Need | File |
|---|---|
| Spider-Man commands (all of them) | `src/sm_guest/main.cpp`, `RunCommand` |
| Arkham commands (all of them) | `src/ak_host/devcmd.h` |
| Shared-memory layout | `protocol/arkweb_protocol.h`, `tools/arkweb_proto.py` |
| Streamer options | `python tools\gotham_stream.py --help` |
| How it was found (addresses, formats) | `recon/PHASE0*.md`, `PHASE1.md`, `PHASE2.md` |
