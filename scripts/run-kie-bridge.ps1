<#
  Sets up the Python environment for the Context <-> Kie.ai bridge (used by run-kie-bridge.bat,
  which then starts the bridge on http://127.0.0.1:3000).

  First run creates tools\kie_bridge\.venv and installs the MP3 decoder (miniaudio), then asks
  for your Kie.ai API key. The key is saved in %APPDATA%\Context Kie Bridge\config.json - never
  in this project folder, the plugin or GitHub.
#>
$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot
$bridgeDir = Join-Path $repoRoot "tools\kie_bridge"
$venv      = Join-Path $bridgeDir ".venv"
$python    = Join-Path $venv "Scripts\python.exe"

if (-not (Test-Path $python)) {
    Write-Host "==> Setting up the bridge (first run only)" -ForegroundColor Cyan
    $py = Get-Command py -ErrorAction SilentlyContinue
    if ($py) { py -3 -m venv $venv } else { python -m venv $venv }
    if (-not (Test-Path $python)) {
        Write-Host "Couldn't create a Python environment. Install Python 3.10+ from python.org (tick 'Add Python to PATH')." -ForegroundColor Red
        exit 1
    }
    $ErrorActionPreference = "Continue"
    & $python -m pip install --disable-pip-version-check -q -r (Join-Path $bridgeDir "requirements.txt")
    if ($LASTEXITCODE -ne 0) {
        Write-Host "Couldn't install the MP3 decoder. The bridge still works when Kie.ai's WAV conversion succeeds." -ForegroundColor Yellow
    }
    $ErrorActionPreference = "Stop"
}

exit 0
