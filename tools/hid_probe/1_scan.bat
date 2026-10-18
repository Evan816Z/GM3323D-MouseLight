@echo off
setlocal
set "EXE=%~dp0hid_probe.exe"
if not exist "%EXE%" (
    echo ERROR: hid_probe.exe not found at:
    echo   %EXE%
    echo Keep all files of this folder together and try again.
    pause
    exit /b 1
)
echo Running: %EXE%
"%EXE%"
echo.
echo === DONE - copy everything in this window and send it back ===
pause
