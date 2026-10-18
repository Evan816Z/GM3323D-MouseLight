@echo off
setlocal enabledelayedexpansion
rem ============================================================
rem  GM3323D Mouse Light Control - one-click Release x64 build
rem  Auto-detects: MinGW-w64 (g++) / LLVM-MinGW (clang++) / MSVC (cl)
rem  Output: dist\MouseLight.exe
rem ============================================================

set "OUTDIR=%~dp0dist"
set "SRC=%~dp0src\main.cpp"
if not exist "%OUTDIR%" mkdir "%OUTDIR%"

echo [build] Looking for a C++ compiler...

rem ---- 1) MinGW-w64 g++ (in PATH or common locations) ----
set "GPP="
for %%C in (g++ x86_64-w64-mingw32-g++) do (
    if not defined GPP (
        where %%C >nul 2>&1 && set "GPP=%%C"
    )
)
if not defined GPP (
    for %%P in (
        "%LOCALAPPDATA%\Microsoft\WinGet\Packages\BrechtSanders.WinLibs.POSIX.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe\mingw64\bin\g++.exe"
        "C:\msys64\mingw64\bin\g++.exe"
        "C:\mingw64\bin\g++.exe"
        "C:\w64devkit\bin\g++.exe"
        "C:\Program Files\mingw64\bin\g++.exe"
    ) do (
        if not defined GPP if exist %%~P set "GPP=%%~P"
    )
)

if defined GPP (
    echo [build] Using MinGW g++: %GPP%
    rem  -static            : static link, no runtime DLLs needed
    rem  -s / -gc-sections  : strip + drop unused code (~290 KB)

    rem  compile icon/resource (windres lives next to g++)
    set "WINDRES="
    for %%C in (windres x86_64-w64-mingw32-windres) do (
        if not defined WINDRES ( where %%C >nul 2>&1 && set "WINDRES=%%C" )
    )
    if not defined WINDRES (
        for /f "delims=" %%i in ("%GPP%") do (
            if exist "%%~dpiwindres.exe" set "WINDRES=%%~dpiwindres.exe"
        )
    )
    set "RES="
    if defined WINDRES (
        if not exist "%~dp0build" mkdir "%~dp0build"
        "%WINDRES%" --use-temp-file -I "%~dp0" "%~dp0app.rc" -O coff -o "%~dp0build\app_res.o" 2>nul
        if not exist "%~dp0build\app_res.o" (
            "%WINDRES%" -I "%~dp0" "%~dp0app.rc" -O coff -o "%~dp0build\app_res.o" 2>nul
        )
        if exist "%~dp0build\app_res.o" set "RES=%~dp0build\app_res.o"
    )
    if not defined RES echo [build] warning: windres not found, building without app icon

    "%GPP%" -O2 -std=c++17 -static -mwindows -s -ffunction-sections -fdata-sections ^
        -Wl,--gc-sections "%SRC%" %RES% -o "%OUTDIR%\MouseLight.exe" ^
        -lsetupapi -lhid -lcomdlg32 -lshell32 -ladvapi32 -lgdi32 -luser32
    goto :check
)

rem ---- 2) clang++ ----
set "CPP="
where clang++ >nul 2>&1 && set "CPP=clang++"
if not defined CPP if exist "C:\Program Files\LLVM\bin\clang++.exe" set "CPP=C:\Program Files\LLVM\bin\clang++.exe"
if defined CPP (
    echo [build] Using clang++: %CPP%
    "%CPP%" -O2 -std=c++17 -target x86_64-pc-windows-msvc -Wl,/subsystem:windows "%SRC%" ^
        -o "%OUTDIR%\MouseLight.exe" -lsetupapi -lhid -lcomdlg32 -lshell32 -ladvapi32 -lgdi32 -luser32 -lole32
    goto :check
)

rem ---- 3) MSVC cl.exe ----
where cl >nul 2>&1
if %errorlevel%==0 (
    echo [build] Using MSVC cl.exe
    cl /nologo /O2 /std:c++17 /EHsc /MT /D_WIN32_WINNT=0x0A00 "%SRC%" ^
       /Fe:"%OUTDIR%\MouseLight.exe" /link /SUBSYSTEM:WINDOWS ^
       setupapi.lib hid.lib comdlg32.lib shell32.lib advapi32.lib gdi32.lib user32.lib ole32.lib
    goto :check
)

echo [build] ERROR: No supported compiler found.
echo         Install one of:  winget install BrechtSanders.WinLibs.POSIX.UCRT
echo                          winget install MartinStorsjo.LLVM-MinGW.UCRT
echo         or run this from a "x64 Native Tools Command Prompt for VS".
exit /b 1

:check
if exist "%OUTDIR%\MouseLight.exe" (
    echo [build] OK -^> %OUTDIR%\MouseLight.exe
    for %%F in ("%OUTDIR%\MouseLight.exe") do echo [build] size: %%~zF bytes
    exit /b 0
)
echo [build] FAILED
exit /b 1
