@echo off
REM Run drift's regression checks on Windows with MSVC.
REM The Unix equivalent is tests/run_tests.sh.
REM
REM Usage: tests\run_tests.bat
setlocal
cd /d "%~dp0"

REM Already in a Developer Command Prompt? Then cl is on PATH -- just run.
where cl >nul 2>&1
if not errorlevel 1 goto :run

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

call "%VSINSTALL%\VC\Auxiliary\Build\vcvars64.bat" >nul
where cl >nul 2>&1
if errorlevel 1 (
    echo Failed to initialize the MSVC build environment.
    exit /b 1
)

:run
set "status=0"
set "OUT=%TEMP%\drift_tests"
if not exist "%OUT%" mkdir "%OUT%"

echo === 1/10  source lint: WriteToBuffer row guards ===
cl /nologo /W4 lint_row_guards.c /Fe:"%OUT%\lint.exe" /Fo:"%OUT%\lint.obj" >nul
if errorlevel 1 (
    echo   lint build FAILED
    set "status=1"
) else (
    "%OUT%\lint.exe" ..\drift.c
    if errorlevel 1 set "status=1"
)
echo.

echo === 2/10  regression tests under AddressSanitizer ===
cl /nologo /W4 /fsanitize=address /Zi row_guard_test.c /Fe:"%OUT%\row_guard_test.exe" /Fo:"%OUT%\row_guard_test.obj" /Fd:"%OUT%\rgt.pdb" >nul
if errorlevel 1 (
    echo   test build FAILED
    set "status=1"
) else (
    "%OUT%\row_guard_test.exe"
    if errorlevel 1 set "status=1"
)
echo.

echo === 3/10  path-aware settings JSON regression tests ===
cl /nologo /W4 /WX /wd4459 /fsanitize=address /Zi settings_json_test.c /Fe:"%OUT%\settings_json_test.exe" /Fo:"%OUT%\settings_json_test.obj" /Fd:"%OUT%\sjt.pdb" >nul
if errorlevel 1 (
    echo   test build FAILED
    set "status=1"
) else (
    "%OUT%\settings_json_test.exe"
    if errorlevel 1 set "status=1"
)
echo.

echo === 4/10  anchor-aware workspace membership path regression tests ===
cl /nologo /W4 /WX /wd4459 /fsanitize=address /Zi membership_path_test.c /Fe:"%OUT%\membership_path_test.exe" /Fo:"%OUT%\membership_path_test.obj" /Fd:"%OUT%\mpt.pdb" >nul
if errorlevel 1 (
    echo   test build FAILED
    set "status=1"
) else (
    "%OUT%\membership_path_test.exe"
    if errorlevel 1 set "status=1"
)
echo.

echo === 5/10  concurrent workspace membership regression tests ===
cl /nologo /W4 /WX /wd4459 /fsanitize=address /Zi membership_concurrency_test.c /Fe:"%OUT%\membership_concurrency_test.exe" /Fo:"%OUT%\membership_concurrency_test.obj" /Fd:"%OUT%\mct.pdb" >nul
if errorlevel 1 (
    echo   test build FAILED
    set "status=1"
) else (
    "%OUT%\membership_concurrency_test.exe"
    if errorlevel 1 set "status=1"
)
echo.

echo === 6/10  fail-closed name metadata regression tests ===
cl /nologo /W4 /WX /wd4459 /fsanitize=address /Zi name_entry_test.c /Fe:"%OUT%\name_entry_test.exe" /Fo:"%OUT%\name_entry_test.obj" /Fd:"%OUT%\net.pdb" >nul
if errorlevel 1 (
    echo   test build FAILED
    set "status=1"
) else (
    "%OUT%\name_entry_test.exe"
    if errorlevel 1 set "status=1"
)
echo.

echo === 7/10  resize-safe session-delete modal regression tests ===
cl /nologo /W4 /WX /wd4459 /fsanitize=address /Zi session_delete_test.c /Fe:"%OUT%\session_delete_test.exe" /Fo:"%OUT%\session_delete_test.obj" /Fd:"%OUT%\sdt.pdb" >"%OUT%\session_delete_build.log" 2>&1
if errorlevel 1 (
    type "%OUT%\session_delete_build.log"
    echo   test build FAILED
    set "status=1"
) else (
    "%OUT%\session_delete_test.exe"
    if errorlevel 1 set "status=1"
)
echo.

echo === 8/10  secure Claude launcher regression tests ===
cl /nologo /W4 /wd4459 /fsanitize=address /Zi claude_launcher_test.c /Fe:"%OUT%\claude_launcher_test.exe" /Fo:"%OUT%\claude_launcher_test.obj" /Fd:"%OUT%\clt.pdb" >nul
if errorlevel 1 (
    echo   test build FAILED
    set "status=1"
) else (
    "%OUT%\claude_launcher_test.exe"
    if errorlevel 1 set "status=1"
)
echo.

echo === 9/10  secure Vim resolver regression tests ===
cl /nologo /W4 /WX /wd4459 /fsanitize=address /Zi vim_resolver_test.c /Fe:"%OUT%\vim_resolver_test.exe" /Fo:"%OUT%\vim_resolver_test.obj" /Fd:"%OUT%\vrt.pdb" >nul
if errorlevel 1 (
    echo   test build FAILED
    set "status=1"
) else (
    "%OUT%\vim_resolver_test.exe"
    if errorlevel 1 set "status=1"
)
echo.

REM /WX turns any new warning into a failure. C4459 is the one known and
REM accepted warning: GetSelectedRowPath and GetFilePath take parameters
REM deliberately named after the globals they shadow.
echo === 10/10  compile drift.c with full warnings ===
cl /nologo /W4 /WX /wd4459 /c ..\drift.c /Fo:"%OUT%\drift_warncheck.obj" >nul
if errorlevel 1 (
    echo   FAILED: drift.c produced a warning beyond the known C4459 shadowing
    set "status=1"
) else (
    echo   no warnings beyond the known C4459 shadowing
)
echo.

if "%status%"=="0" (
    echo ALL CHECKS PASSED
) else (
    echo CHECKS FAILED
)
exit /b %status%
