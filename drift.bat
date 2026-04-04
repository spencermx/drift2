@echo off
drift.exe
for /f "delims=" %%i in (%TEMP%\browser_lastdir.txt) do cd /d "%%i"
REM call "%userprofile%\_refresh_prompt.bat"
