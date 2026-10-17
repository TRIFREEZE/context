@echo off
rem Replaces the saved Kie.ai API key, checks it and shows your remaining credits.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0run-kie-bridge.ps1"
if errorlevel 1 goto end
"%~dp0..\tools\kie_bridge\.venv\Scripts\python.exe" "%~dp0..\tools\kie_bridge\kie_bridge.py" --set-key --check
:end
pause
