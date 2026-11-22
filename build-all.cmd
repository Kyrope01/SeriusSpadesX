@echo off
rem The whole rebuild, one command: the game compiled (Release) and the
rem paks rebuilt and installed beside the fresh exe - maps, turret models
rem and sounds included. Run from the repo root, or double-click:
rem
rem   build-all.cmd
rem
rem One first configure of the build folder is needed (README.md, How to
rem Build); everything after that is this command.
setlocal
where cmake >nul 2>nul
if errorlevel 1 (
  echo cmake not found in PATH: open a Visual Studio command prompt first.
  exit /b 1
)
if not exist "%~dp0build" (
  echo No build folder yet: configure once first, README.md, How to Build.
  exit /b 1
)
echo == Building the game, Release
cmake --build "%~dp0build" --config Release --parallel
if errorlevel 1 (
  echo Build failed.
  exit /b 1
)
echo == Rebuilding and installing the paks
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0Resources\rebuild-paks.ps1"
if errorlevel 1 (
  echo Paks failed.
  exit /b 1
)
echo == Done: launch the game from the folder named above - exe, paks and
echo    settings in one place, nothing anywhere else.
