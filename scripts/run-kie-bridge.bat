@echo off
rem Starts the Context <-> Kie.ai bridge on http://127.0.0.1:3000 (real AI audio via your Kie.ai account).
rem First run sets up Python packages and asks for your Kie.ai API key. Leave the window open while you use Context.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0run-kie-bridge.ps1"
if errorlevel 1 goto end
echo ==^> Kie.ai bridge (close this window to stop it)
"%~dp0..\tools\kie_bridge\.venv\Scripts\python.exe" "%~dp0..\tools\kie_bridge\kie_bridge.py" %*
:end
pause
