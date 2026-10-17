@echo off
rem Starts the Context REFERENCE bridge on http://127.0.0.1:3000 (synthetic test audio, no AI provider,
rem no credentials). Use it to check that Context's bridge connection works end to end.
cd /d "%~dp0.."
where py >nul 2>nul && (py -3 tools\reference_bridge\reference_bridge.py --port 3000) || (python tools\reference_bridge\reference_bridge.py --port 3000)
pause
