@echo off
rem One-command local Windows build. Arguments are passed through, e.g.  build-windows.bat -SkipTests
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build-windows.ps1" %*
if errorlevel 1 (
  echo.
  echo Build failed. Copy the red error text above and paste it into Claude.
  pause
  exit /b 1
)
pause
