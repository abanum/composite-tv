@echo off
rem ===================================================================
rem  build-installer.bat  --  double-click to build the Windows installer
rem
rem  Runs build-installer.ps1, which does:
rem    cmake --build  ->  cmake --install  ->  Inno Setup (ISCC)
rem  and writes  release\composite-tv-<version>-windows-x64.exe
rem
rem  Prerequisites (one time):
rem    * cmake --preset windows-x64      (configure the project once)
rem    * Inno Setup 6 installed          (https://jrsoftware.org/isdl.php)
rem ===================================================================
setlocal

rem Repo root = this script's folder, one level up.
pushd "%~dp0.."

powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build-installer.ps1" %*
set "RC=%ERRORLEVEL%"

popd

echo.
if "%RC%"=="0" (
    echo [OK] Installer build finished.
) else (
    echo [FAILED] exit code %RC%
)
echo.
pause
exit /b %RC%
