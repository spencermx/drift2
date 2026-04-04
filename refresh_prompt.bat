@echo off

:: Check if we're in a git repo
git rev-parse --is-inside-work-tree >nul 2>&1
if %ERRORLEVEL% neq 0 (
    :: Not a git repo — clean prompt
    set "PROMPT=%CYAN%$P%RESET% %DIM%$G%RESET% "
    goto :eof
)

:: Get the current branch name
for /F "tokens=*" %%b in ('git rev-parse --abbrev-ref HEAD 2^>nul') do set "GIT_BRANCH=%%b"

:: Check for uncommitted changes
git diff --quiet --exit-code >nul 2>&1
if %ERRORLEVEL% neq 0 (
    set "DIRTY=%YELLOW%*%RESET%"
) else (
    set "DIRTY="
)

:: Set prompt with branch info
set "PROMPT=%CYAN%$P %MAGENTA%(%GIT_BRANCH%%DIRTY%%MAGENTA%)%RESET% %DIM%$G%RESET% "
