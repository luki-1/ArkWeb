@echo off
cd /d "%~dp0"
call "C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul
ml64 /nologo /c target.asm || exit /b 1
cl /nologo /O2 /MT /EHsc /std:c++17 hooktest.cpp target.obj psapi.lib || exit /b 1
start "" /low /affinity 1 /wait /b hooktest.exe
