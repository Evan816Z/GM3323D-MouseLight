@echo off
rem GM3323D config-watch launcher (ASCII only; run then pause so output stays visible)
setlocal
cd /d "%~dp0"
echo ============================================
echo  GM3323D config-watch probe
echo  After it starts, change a DPI value in the original software to trigger capture.
echo  Stops automatically after 4 captures, or press ESC to stop early.
echo ============================================
echo.
config_watch.exe
echo.
echo [done] log saved as config_watch_*.txt in this folder
pause
