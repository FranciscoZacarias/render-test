@echo off
setlocal

REM Require a commit message (must be quoted if it has spaces)
if "%~1"=="" (
  echo Error: No commit message provided.
  echo Usage: %~nx0 "your commit message"
  exit /b 1
)

git add .
git commit -m "%~1"
git push

endlocal