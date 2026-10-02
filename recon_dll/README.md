# ArkWeb recon DLLs

These are passive Phase 0b probes. Each game gets a proxy DLL that forwards every export to the
real system DLL.

| DLL | Game | Does |
|---|---|---|
| `out/sm/winmm.dll` | Spider-Man Remastered (next to `Spider-Man.exe`) | Hooks `hknpWorld::castRay/castShape/getClosestPoints` to count calls and sample them. Never changes results. |
| `out/ak/dinput8.dll` | Arkham Knight (`Binaries/Win64`) | Read-only: dumps the name table, object table and PhysX info on request. |

- **Output** goes to `ArkWeb/logs/`: `sm_recon.log`, `ak_recon.log`, plus any `.bin`/`.txt` dumps.
- **Live commands:** write lines to `logs/sm_cmd.txt` or `logs/ak_cmd.txt`. The DLL runs them
  within half a second, logs the results and deletes the file. The command list is in
  `common/recon.h` and at the top of each `.cpp`.
- **Build:** `build.bat` (VS2019 BuildTools x64, low priority, one core).
- **Install / remove:** `install.bat` / `uninstall.bat`.
- `test/` holds an offline test of the hook engine and the command file (`build_test.bat`).
