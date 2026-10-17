<#
  Starts the Context licensing backend on http://127.0.0.1:8787 for testing.

    powershell -ExecutionPolicy Bypass -File scripts\run-license-server.ps1

  First run creates backend\.venv and installs the pinned Python packages.
  The private signing key must be at backend\keys\signing_key.pem (never commit it).
#>
$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot
$backend  = Join-Path $repoRoot "backend"
$venv     = Join-Path $backend ".venv"
$python   = Join-Path $venv "Scripts\python.exe"

if (-not (Test-Path $python)) {
    Write-Host "==> Creating Python environment (first run only)" -ForegroundColor Cyan
    $py = Get-Command py -ErrorAction SilentlyContinue
    if ($py) { py -3 -m venv $venv } else { python -m venv $venv }
    & $python -m pip install --upgrade pip
    & $python -m pip install -r (Join-Path $backend "requirements.txt")
}

$key = Join-Path $backend "keys\signing_key.pem"
if (-not (Test-Path $key)) {
    Write-Host ""
    Write-Host "No signing key at $key" -ForegroundColor Yellow
    Write-Host "Either copy the signing_key.pem you were given into backend\keys\ ,"
    Write-Host "or create a NEW key pair (then commit licensing\license_public_key.txt and rebuild the plugin):"
    Write-Host "    cd backend; .venv\Scripts\python -m app.admin init-keys"
    exit 1
}

Write-Host "==> Licensing server: http://127.0.0.1:8787   (Ctrl+C to stop)" -ForegroundColor Green
Push-Location $backend
try { & $python -m uvicorn app.main:app --host 127.0.0.1 --port 8787 }
finally { Pop-Location }
