@echo off
setlocal
REM Run from the repository root so the src\ and build\ paths below resolve.
cd /d "%~dp0.."

if not exist src\drift.c (
    echo src\drift.c not found.
    exit /b 1
)

REM Already in a Developer Command Prompt? Then cl is on PATH -- just build.
where cl >nul 2>&1
if not errorlevel 1 goto :build

REM Locate Visual Studio via vswhere, which ships with every VS 2017+ install
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo Visual Studio was not found ^(vswhere.exe is missing^).
    echo Install Visual Studio with the "Desktop development with C++" workload:
    echo   https://visualstudio.microsoft.com/downloads/
    exit /b 1
)

set "VSINSTALL="
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSINSTALL=%%i"
if not defined VSINSTALL (
    echo Visual Studio is installed, but the C++ compiler is not.
    echo Open "Visual Studio Installer", add the "Desktop development with C++"
    echo workload, then re-run this script.
    exit /b 1
)

echo Using Visual Studio at: %VSINSTALL%
REM vcvars is noisy on stderr too (VS 18's VsDevCmd.bat fails to locate
REM vswhere.exe when NoDefaultCurrentDirectoryInExePath is set). The
REM `where cl` check below is what actually decides whether it worked.
call "%VSINSTALL%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
where cl >nul 2>&1
if errorlevel 1 (
    echo Failed to initialize the MSVC build environment.
    exit /b 1
)

:build
if not exist build mkdir build

REM Drop the previous binary first. cl fails before overwriting it, so without
REM this a failed build leaves a stale drift.exe that looks like a fresh one.
del build\drift.exe >nul 2>&1
if exist build\drift.exe (
    echo Cannot replace build\drift.exe -- is it still running?
    exit /b 1
)

echo Building drift.exe...
cl /nologo /W3 /O2 src\drift.c /Fe:build\drift.exe /Fo:build\drift.obj shell32.lib
if errorlevel 1 (
    echo.
    echo Build FAILED.
    exit /b 1
)
del build\drift.obj >nul 2>&1
echo.
echo Build succeeded: %CD%\build\drift.exe

REM No install step. build\ is itself on PATH -- the windows setup repo adds
REM this directory rather than copying the binary out of it, so a fresh build
REM is immediately the drift you get when you type `drift`. One binary, one
REM location, no question of which one is running.

exit /b 0
