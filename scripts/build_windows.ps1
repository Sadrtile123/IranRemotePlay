# RemotePlay - scripts/build_windows.ps1
# One-shot configure + build + test for Visual Studio 2022 / Qt 6.
# Usage:
#   .\scripts\build_windows.ps1 -QtDir "C:\Qt\6.8.1\msvc2022_64"
param(
    [Parameter(Mandatory = $true)]
    [string]$QtDir
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path (Join-Path $QtDir "lib\cmake\Qt6"))) {
    Write-Error "QtDir does not look like a Qt MSVC 64-bit install (missing lib\cmake\Qt6): $QtDir"
}

Write-Host "== Configuring =="
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 "-DCMAKE_PREFIX_PATH=$QtDir"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host "== Building (Release) =="
cmake --build build --config Release
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host "== Running tests =="
ctest --test-dir build -C Release --output-on-failure
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host ""
Write-Host "Build OK. Binary: build\bin\Release\RemotePlay.exe"
Write-Host "Optional (collect Qt DLLs next to the exe):"
Write-Host "  $QtDir\bin\windeployqt.exe build\bin\Release\RemotePlay.exe"
