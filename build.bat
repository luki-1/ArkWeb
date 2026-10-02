@echo off
rem Builds the ArkWeb DLLs and tests into bin\ (one compiler at a time, low priority, one core).
setlocal
cd /d "%~dp0"
call "C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul || exit /b 1
for %%d in (bin bin\obj\ak bin\obj\sm bin\obj\test bin\ak bin\sm) do if not exist %%d mkdir %%d
del /q bin\ak\dinput8.dll bin\sm\winmm.dll bin\link_test.exe src\sm_guest\capture_cs.h src\ak_host\overlay_vs.h src\ak_host\overlay_ps.h 2>nul
set FXC=C:\Program Files (x86)\Windows Kits\10\bin\10.0.19041.0\x64\fxc.exe
start "" /low /affinity 1 /wait /b "%FXC%" /nologo /T cs_5_0 /E main /O3 /Vn g_captureCs /Fh src\sm_guest\capture_cs.h src\sm_guest\capture_cs.hlsl
if not exist src\sm_guest\capture_cs.h exit /b 1
start "" /low /affinity 1 /wait /b "%FXC%" /nologo /T vs_5_0 /E vs /O3 /Vn g_overlayVs /Fh src\ak_host\overlay_vs.h src\ak_host\overlay.hlsl
if not exist src\ak_host\overlay_vs.h exit /b 1
start "" /low /affinity 1 /wait /b "%FXC%" /nologo /T ps_5_0 /E ps /O3 /Vn g_overlayPs /Fh src\ak_host\overlay_ps.h src\ak_host\overlay.hlsl
if not exist src\ak_host\overlay_ps.h exit /b 1
set OPTS=/nologo /O2 /MT /EHsc /std:c++17 /W3 /D_CRT_SECURE_NO_WARNINGS /DNOMINMAX
start "" /low /affinity 1 /wait /b cl %OPTS% /LD src\ak_host\main.cpp /Fobin\obj\ak\ /Febin\ak\dinput8.dll /link user32.lib
if not exist bin\ak\dinput8.dll exit /b 1
start "" /low /affinity 1 /wait /b cl %OPTS% /LD src\sm_guest\main.cpp /Fobin\obj\sm\ /Febin\sm\winmm.dll /link user32.lib advapi32.lib
if not exist bin\sm\winmm.dll exit /b 1
start "" /low /affinity 1 /wait /b cl %OPTS% tests\link_test.cpp /Fobin\obj\test\ /Febin\link_test.exe
if not exist bin\link_test.exe exit /b 1
echo BUILD OK
