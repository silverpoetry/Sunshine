@echo off
setlocal

set "SCRIPT_DIR=%~dp0"
set "SCRIPT=%SCRIPT_DIR%Install-LocalProgramFilesSunshine.ps1"
set "MSI=%SCRIPT_DIR%SunshineInstaller-Current.msi"

if exist "%MSI%" (
    powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%SCRIPT%" -MsiPath "%MSI%" %*
) else (
    powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%SCRIPT%" %*
)
pause
