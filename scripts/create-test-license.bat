@echo off
rem Creates a Context license (2 devices) and prints the key. Run run-license-server.bat once first (it sets up Python).
cd /d "%~dp0..\backend"
if not exist ".venv\Scripts\python.exe" (
  echo Run scripts\run-license-server.bat once first so the Python environment exists.
  pause
  exit /b 1
)
.venv\Scripts\python.exe -m app.admin create --max-devices 2 --note "test license"
pause
