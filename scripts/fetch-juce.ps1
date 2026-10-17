# Fetches exactly the JUCE commit pinned in dependencies/JUCE_COMMIT into .deps/JUCE
# using a shallow fetch (fast). Safe to run repeatedly.
#   powershell -ExecutionPolicy Bypass -File scripts\fetch-juce.ps1
$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot
$commit   = (Get-Content (Join-Path $repoRoot "dependencies\JUCE_COMMIT") -TotalCount 1).Trim()
$dest     = Join-Path $repoRoot ".deps\JUCE"

if (-not ($commit -match '^[0-9a-f]{40}$')) { throw "dependencies/JUCE_COMMIT does not contain a full commit hash" }

if (Test-Path (Join-Path $dest ".git")) {
    $current = (git -C $dest rev-parse HEAD 2>$null)
    if ($current -eq $commit) { Write-Host "JUCE $commit already present at $dest"; exit 0 }
    Write-Host "JUCE folder is at a different commit ($current); refreshing."
    Remove-Item -Recurse -Force $dest
}

New-Item -ItemType Directory -Force -Path $dest | Out-Null
git -C $dest init -q
git -C $dest remote add origin https://github.com/juce-framework/JUCE.git
git -C $dest fetch -q --depth 1 origin $commit
if ($LASTEXITCODE -ne 0) { throw "git fetch of JUCE $commit failed" }
git -C $dest checkout -q FETCH_HEAD
if ($LASTEXITCODE -ne 0) { throw "git checkout of JUCE $commit failed" }
Write-Host "JUCE $commit fetched to $dest"
