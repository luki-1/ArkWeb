@echo off
rem Installs the ArkWeb DLLs (bin\) into both games. uninstall.bat removes them.
copy /y "%~dp0bin\ak\dinput8.dll" "F:\SteamLibrary\steamapps\common\Batman Arkham Knight\Binaries\Win64\dinput8.dll"
copy /y "%~dp0bin\sm\winmm.dll" "H:\SteamLibrary\steamapps\common\Marvel's Spider-Man Remastered\winmm.dll"
