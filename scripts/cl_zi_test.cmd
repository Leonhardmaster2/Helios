@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cd /d C:\Users\leonh\Documents\Helios\.tools
echo int main(){return 0;} > t1.c
echo === Zi ===
cl /nologo /Zi /c t1.c /Fot1_zi.obj > zi_out.txt 2>&1
echo zi exit: %ERRORLEVEL%
echo === Z7 ===
cl /nologo /Z7 /c t1.c /Fot1_z7.obj > z7_out.txt 2>&1
echo z7 exit: %ERRORLEVEL%
dir t1_*.obj 2>nul
del t1.c t1_zi.obj t1_z7.obj zi_out.txt z7_out.txt 2>nul
