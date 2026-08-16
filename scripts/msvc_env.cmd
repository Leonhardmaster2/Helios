@echo off
REM Loads the MSVC x64 toolchain environment and adds local tools to PATH.
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
set PATH=C:\Users\leonh\Documents\Helios\.tools;%PATH%
