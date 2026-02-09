@echo off
drift.exe
for /f "delims=" %%i in (%TEMP%\browser_lastdir.txt) do cd /d "%%i"
