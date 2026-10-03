@echo off
rem Installs the ArkWeb DLLs (bin\) into both games. uninstall.bat removes them.
rem Each DLL also gets an arkweb.ini (existing settings are kept) whose LogDir is this folder's logs\:
rem the streamer and both games exchange files there (scans, PhysX exports, grapple points).
setlocal
set "ROOT=%~dp0"
set "AK=F:\SteamLibrary\steamapps\common\Batman Arkham Knight\Binaries\Win64"
set "SM=H:\SteamLibrary\steamapps\common\Marvel's Spider-Man Remastered"
copy /y "%ROOT%bin\ak\dinput8.dll" "%AK%\dinput8.dll"
copy /y "%ROOT%bin\sm\winmm.dll" "%SM%\winmm.dll"
set "INI=%AK%\arkweb.ini"
if not exist "%INI%" echo [ArkWeb]>"%INI%"
findstr /b /i "LogDir=" "%INI%" >nul || echo.>>"%INI%"
findstr /b /i "LogDir=" "%INI%" >nul || echo LogDir=%ROOT%logs>>"%INI%"
set "INI=%SM%\arkweb.ini"
if not exist "%INI%" echo [ArkWeb]>"%INI%"
findstr /b /i "LogDir=" "%INI%" >nul || echo.>>"%INI%"
findstr /b /i "LogDir=" "%INI%" >nul || echo LogDir=%ROOT%logs>>"%INI%"
