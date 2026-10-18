@echo off
rem ============================================================
rem  Remove all build output and intermediate files.
rem  Keeps: source, docs, scripts, resources.
rem ============================================================
echo [clean] Removing build output and intermediates...

if exist "%~dp0dist" rmdir /s /q "%~dp0dist"
if exist "%~dp0build" rmdir /s /q "%~dp0build"
if exist "%~dp0obj" rmdir /s /q "%~dp0obj"

del /s /q "%~dp0*.o"    2>nul
del /s /q "%~dp0*.obj"  2>nul
del /s /q "%~dp0*.ilk"  2>nul
del /s /q "%~dp0*.pdb"  2>nul
del /s /q "%~dp0*.exp"  2>nul
del /s /q "%~dp0*.lib"  2>nul

echo [clean] Done.
echo [clean] Rebuild with:  build.bat
