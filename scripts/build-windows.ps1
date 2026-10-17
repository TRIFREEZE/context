<#
  One-command local Windows build of Context (Release VST3 + Standalone + tests).

    powershell -ExecutionPolicy Bypass -File scripts\build-windows.ps1
    powershell -ExecutionPolicy Bypass -File scripts\build-windows.ps1 -SkipTests
    powershell -ExecutionPolicy Bypass -File scripts\build-windows.ps1 -Install     (copies Context.vst3 into C:\Program Files\Common Files\VST3; run as Administrator)

  Needs: Visual Studio 2022 or newer with "Desktop development with C++", CMake 3.22+, Git, Python 3.10+.
  GitHub Actions is still the easiest way to get a build: see README.md.
#>
param(
    [string]$Config = "Release",
    [switch]$SkipTests,
    [switch]$Install
)
$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot
$buildDir = Join-Path $repoRoot "build"
$product  = "Context"

function Require($cmd, $hint) {
    if (-not (Get-Command $cmd -ErrorAction SilentlyContinue)) { throw "$cmd was not found. $hint" }
}
Require git   "Install Git for Windows: https://git-scm.com/download/win"
Require cmake "Install CMake (or the Visual Studio 'C++ CMake tools' component): https://cmake.org/download/"

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere) -or -not (& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath)) {
    throw "Visual Studio with the 'Desktop development with C++' workload was not found."
}

Write-Host "==> Fetching pinned JUCE" -ForegroundColor Cyan
& (Join-Path $PSScriptRoot "fetch-juce.ps1")

Write-Host "==> Configuring" -ForegroundColor Cyan
cmake -S $repoRoot -B $buildDir -A x64 "-DFETCHCONTENT_SOURCE_DIR_JUCE=$repoRoot/.deps/JUCE" -DCONTEXT_BUILD_TESTS=ON
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed" }

Write-Host "==> Building ($Config)" -ForegroundColor Cyan
cmake --build $buildDir --config $Config --parallel
if ($LASTEXITCODE -ne 0) { throw "Build failed (see the compiler output above)" }

if (-not $SkipTests) {
    Write-Host "==> Running tests" -ForegroundColor Cyan
    ctest --test-dir $buildDir -C $Config --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw "Tests failed" }
}

$bundle = Join-Path $buildDir "$($product)_artefacts\$Config\VST3\$product.vst3"
if (-not (Test-Path $bundle)) { throw "$product.vst3 was not produced at $bundle" }
Write-Host ""
Write-Host "Built: $bundle" -ForegroundColor Green

if ($Install) {
    $dest = Join-Path ${env:CommonProgramFiles} "VST3\$product.vst3"
    Write-Host "==> Installing to $dest" -ForegroundColor Cyan
    if (Test-Path $dest) { Remove-Item -Recurse -Force $dest }   # replaces only Context.vst3, nothing else
    Copy-Item -Recurse $bundle $dest
    Write-Host "Installed. Rescan plugins in your DAW." -ForegroundColor Green
}
