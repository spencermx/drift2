@echo off

for /F %%a in ('echo prompt $E ^| cmd') do set "ESC=%%a"
set "RESET=%ESC%[0m"
set "CYAN=%ESC%[36m"
set "MAGENTA=%ESC%[35m"
set "YELLOW=%ESC%[33m"

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
    set "BRANCH_COLOR=%YELLOW%"
) else (
    set "BRANCH_COLOR="
)

:: Set prompt with branch info
set "PROMPT=$P %BRANCH_COLOR%(%GIT_BRANCH%)%RESET% %DIM%$G%RESET% "
