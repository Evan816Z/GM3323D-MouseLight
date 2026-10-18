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
if "%1"=="__ELEVATED" goto run
net session >nul 2>&1
if %errorlevel%==0 goto run
echo Requesting administrator rights - click YES on the UAC prompt...
set "PB=%~f0"
powershell -NoProfile -Command "Start-Process cmd.exe -ArgumentList '/c',$env:PB,'__ELEVATED' -Verb RunAs"
exit /b
:run
echo Running: %EXE%
"%EXE%"
echo.
echo === DONE ^(admin^) - copy everything in this window and send it back ===
pause
