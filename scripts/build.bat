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
call "%VSINSTALL%\VC\Auxiliary\Build\vcvars64.bat" >nul
where cl >nul 2>&1
if errorlevel 1 (
    echo Failed to initialize the MSVC build environment.
    exit /b 1
)

:build
if not exist build mkdir build
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
exit /b 0
