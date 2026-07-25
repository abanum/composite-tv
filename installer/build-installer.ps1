<#
    build-installer.ps1 - build the plugin and produce a Windows installer (.exe)

    Prerequisites:
      * The project has been configured once:  cmake --preset windows-x64
      * Inno Setup 6 is installed (free): https://jrsoftware.org/isdl.php

    Usage (from the repo root or anywhere):
      powershell -ExecutionPolicy Bypass -File installer\build-installer.ps1
      # optional: -Configuration Release   (default: RelWithDebInfo)

    NOTE: keep this file ASCII-only. Windows PowerShell 5.1 reads a BOM-less
    script as the system ANSI code page, so non-ASCII text corrupts parsing.
#>
[CmdletBinding()]
param(
    [string]$Configuration = 'RelWithDebInfo'
)

$ErrorActionPreference = 'Stop'

# Repo root = parent of this script's folder.
$Root = Split-Path -Parent $PSScriptRoot
Set-Location $Root

# --- version from buildspec.json -----------------------------------------
$spec = Get-Content -Raw -Encoding UTF8 (Join-Path $Root 'buildspec.json') | ConvertFrom-Json
$version = $spec.version
if ([string]::IsNullOrWhiteSpace($version)) { throw "version missing in buildspec.json" }
Write-Host "NTSC Snow installer build - version $version ($Configuration)" -ForegroundColor Cyan

# --- build + install tree -------------------------------------------------
Write-Host "==> cmake --build" -ForegroundColor Green
cmake --build --preset windows-x64 --config $Configuration
if ($LASTEXITCODE -ne 0) { throw "cmake build failed ($LASTEXITCODE)" }

$prefix = Join-Path $Root "release\$Configuration"
Write-Host "==> cmake --install -> $prefix" -ForegroundColor Green
cmake --install build_x64 --prefix $prefix --config $Configuration
if ($LASTEXITCODE -ne 0) { throw "cmake install failed ($LASTEXITCODE)" }

$dll = Join-Path $prefix 'ntsc-snow\bin\64bit\ntsc-snow.dll'
if (-not (Test-Path $dll)) { throw "expected payload not found: $dll" }

# --- locate ISCC.exe (Inno Setup compiler) -------------------------------
function Find-ISCC {
    try {
        $loc = (Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\Inno Setup 6_is1' -ErrorAction Stop).InstallLocation
        if ($loc) {
            $p = Join-Path $loc 'ISCC.exe'
            if (Test-Path $p) { return $p }
        }
    } catch {}
    foreach ($p in @(
            "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
            "${env:ProgramFiles}\Inno Setup 6\ISCC.exe")) {
        if ($p -and (Test-Path $p)) { return $p }
    }
    $cmd = Get-Command ISCC.exe -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    return $null
}

$iscc = Find-ISCC
if (-not $iscc) {
    Write-Host ""
    Write-Warning "Inno Setup compiler (ISCC.exe) not found."
    Write-Host    "  Install the free Inno Setup 6: https://jrsoftware.org/isdl.php" -ForegroundColor Yellow
    Write-Host    "  Then run this script again." -ForegroundColor Yellow
    exit 1
}
Write-Host "==> ISCC: $iscc" -ForegroundColor Green

# --- compile installer ----------------------------------------------------
$iss = Join-Path $PSScriptRoot 'ntsc-snow.iss'
& $iscc "/DAppVersion=$version" $iss
if ($LASTEXITCODE -ne 0) { throw "ISCC failed ($LASTEXITCODE)" }

$out = Join-Path $Root "release\ntsc-snow-$version-windows-x64.exe"
if (Test-Path $out) {
    Write-Host ""
    Write-Host "DONE: $out" -ForegroundColor Cyan
} else {
    Write-Warning "ISCC reported success but the expected output was not found: $out"
}
