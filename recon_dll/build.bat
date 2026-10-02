@echo off
rem Builds both recon DLLs into recon_dll\out\ (one compiler at a time, low priority).
setlocal
cd /d "%~dp0"
call "C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
if not exist out\sm mkdir out\sm
if not exist out\ak mkdir out\ak
set CL_OPTS=/nologo /O2 /MT /LD /EHsc /std:c++17 /W3 /D_CRT_SECURE_NO_WARNINGS
start "" /low /affinity 1 /wait /b cl %CL_OPTS% sm_recon\sm_recon.cpp /Foout\sm\ /Feout\sm\winmm.dll /link psapi.lib || exit /b 1
start "" /low /affinity 1 /wait /b cl %CL_OPTS% ak_recon\ak_recon.cpp /Foout\ak\ /Feout\ak\dinput8.dll /link psapi.lib || exit /b 1
echo BUILD OK
