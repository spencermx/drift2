@echo off
drift.exe
if exist "%TEMP%\browser_lastdir.txt" (
    for /f "usebackq delims=" %%i in ("%TEMP%\browser_lastdir.txt") do cd /d "%%i"
)
REM call "%userprofile%\_refresh_prompt.bat"
