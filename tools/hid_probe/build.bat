@echo off
REM Build script for hid_probe.exe (diagnostic tool, NOT the final product)
REM Auto-detects: g++ (MinGW-w64 / WinLibs / w64devkit) -> clang++ -> cl
setlocal
set SRC=%~dp0hid_probe.cpp
set OUT=%~dp0hid_probe.exe

set CXX=
where g++ >nul 2>nul && set CXX=g++
if "%CXX%"=="" where clang++ >nul 2>nul && set CXX=clang++
if "%CXX%"=="" where cl >nul 2>nul && set CXX=cl

if "%CXX%"=="" (
    echo No compiler found. Install one of:
    echo   - MinGW-w64 ^(winget install BrechtSanders.WinLibs.POSIX.UCRT^)
    echo   - LLVM-MinGW / w64devkit
    echo   - Visual Studio Build Tools ^(cl.exe^)
    exit /b 1
)
echo Using compiler: %CXX%

if "%CXX%"=="cl" (
    cl /nologo /O2 /EHsc /W3 %SRC% /Fe:%OUT% setupapi.lib hid.lib advapi32.lib
) else (
    %CXX% -O2 -std=c++17 -static -mconsole -municode %SRC% -o %OUT% -lsetupapi -lhid -ladvapi32
)
if errorlevel 1 (
    echo BUILD FAILED
    exit /b 1
)
echo Built: %OUT%
exit /b 0
