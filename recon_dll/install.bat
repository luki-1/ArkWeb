@echo off
rem Copies the recon DLLs into both games. uninstall.bat removes them again.
copy /y "%~dp0out\sm\winmm.dll" "H:\SteamLibrary\steamapps\common\Marvel's Spider-Man Remastered\winmm.dll"
copy /y "%~dp0out\ak\dinput8.dll" "F:\SteamLibrary\steamapps\common\Batman Arkham Knight\Binaries\Win64\dinput8.dll"
