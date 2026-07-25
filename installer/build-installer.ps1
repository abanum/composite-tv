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
    [string]$Configuration = 'RelWithDebInfo',
    [string]$ISCC = ''
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
# Cheap probes first: a default install is found by nine Test-Path calls,
# whereas the registry sweep reads several hundred keys. Inno Setup is a 32-bit
# app, so its uninstall entry lives under WOW6432Node.
function Find-ISCC {
    foreach ($base in @(${env:ProgramFiles(x86)}, $env:ProgramFiles, "$env:LOCALAPPDATA\Programs")) {
        if (-not $base) { continue }
        foreach ($ver in @('Inno Setup 6', 'Inno Setup 5', 'Inno Setup')) {
            $p = Join-Path $base (Join-Path $ver 'ISCC.exe')
            if (Test-Path $p) { return $p }
        }
    }
    $cmd = Get-Command ISCC.exe -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }

    $roots = @(
        'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall',
        'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall',
        'HKCU:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall'
    )
    foreach ($root in $roots) {
        if (-not (Test-Path $root)) { continue }
        foreach ($key in (Get-ChildItem $root -ErrorAction SilentlyContinue)) {
            $props = Get-ItemProperty $key.PSPath -ErrorAction SilentlyContinue
            $isInno = ($key.PSChildName -like 'Inno Setup*_is1') -or ($props.DisplayName -like 'Inno Setup*')
            if ($isInno -and $props.InstallLocation) {
                $p = Join-Path $props.InstallLocation 'ISCC.exe'
                if (Test-Path $p) { return $p }
            }
        }
    }
    return $null
}

# NOTE: PowerShell variable names are case-insensitive, so this must not be
# called $iscc - that would be the -ISCC parameter itself.
$isccExe = if ($ISCC -and (Test-Path $ISCC)) { $ISCC } else { Find-ISCC }
if (-not $isccExe) {
    Write-Host ""
    Write-Warning "Inno Setup compiler (ISCC.exe) not found automatically."
    Write-Host    "  If Inno Setup is installed, pass its path explicitly, e.g.:" -ForegroundColor Yellow
    Write-Host    '    installer\build-installer.bat -ISCC "C:\Program Files (x86)\Inno Setup 6\ISCC.exe"' -ForegroundColor Yellow
    Write-Host    "  Find it with:  where.exe ISCC   (or search for ISCC.exe)" -ForegroundColor Yellow
    Write-Host    "  Or install the free Inno Setup 6: https://jrsoftware.org/isdl.php" -ForegroundColor Yellow
    exit 1
}
Write-Host "==> ISCC: $isccExe" -ForegroundColor Green

# --- compile installer ----------------------------------------------------
$iss = Join-Path $PSScriptRoot 'ntsc-snow.iss'
& $isccExe "/DAppVersion=$version" $iss
if ($LASTEXITCODE -ne 0) { throw "ISCC failed ($LASTEXITCODE)" }

$out = Join-Path $Root "release\ntsc-snow-$version-windows-x64.exe"
if (Test-Path $out) {
    Write-Host ""
    Write-Host "DONE: $out" -ForegroundColor Cyan
} else {
    Write-Warning "ISCC reported success but the expected output was not found: $out"
}
