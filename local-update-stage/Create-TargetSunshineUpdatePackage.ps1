$ErrorActionPreference = 'Stop'

$repo = 'D:\Projects\sunshine'
$buildRoot = Join-Path $repo 'build-release-e-msys2'
$portableRoot = 'C:\Users\weich\Desktop\Tools\SystemTool\sunshine-windows-portable\Sunshine'
$msysZlib = 'E:\Develop\MSYS2\msys64\ucrt64\bin\zlib1.dll'
$transferFolderName = -join @([char]0x7a7f, [char]0x68ad, [char]0x673a)
$transferRoot = Join-Path (Join-Path $env:USERPROFILE 'Desktop') $transferFolderName
$finalPackageRoot = Join-Path $transferRoot 'SunshineUpdate-Current'
$stagingRoot = Join-Path $env:TEMP 'SunshineUpdate-Current-Staging'
$packageRoot = Join-Path $stagingRoot 'SunshineUpdate-Current'
$backupRoot = Join-Path $repo 'local-update-stage\package-backups'

$sourceSunshine = Join-Path $buildRoot 'sunshine.exe'
$sourceHelper = Join-Path $buildRoot 'tools\sunshine-wgc-helper.exe'
$sourceWeb = Join-Path $buildRoot 'assets\web'
$sourceZlibCandidates = @(
    $msysZlib,
    (Join-Path $portableRoot 'zlib1.dll')
)
$sourceZlib = $sourceZlibCandidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
if (!$sourceZlib) {
    throw "Required zlib1.dll not found in known locations: $($sourceZlibCandidates -join ', ')"
}

foreach ($path in @($sourceSunshine, $sourceHelper, $sourceWeb, $sourceZlib)) {
    if (!(Test-Path -LiteralPath $path)) {
        throw "Required package source not found: $path"
    }
}

if (Test-Path -LiteralPath $stagingRoot) {
    Remove-Item -LiteralPath $stagingRoot -Recurse -Force
}
New-Item -ItemType Directory -Path $packageRoot | Out-Null
New-Item -ItemType Directory -Path (Join-Path $packageRoot 'tools') | Out-Null
New-Item -ItemType Directory -Path (Join-Path $packageRoot 'assets') | Out-Null

Copy-Item -LiteralPath $sourceSunshine -Destination (Join-Path $packageRoot 'sunshine.exe') -Force
Copy-Item -LiteralPath $sourceHelper -Destination (Join-Path $packageRoot 'tools\sunshine-wgc-helper.exe') -Force
Copy-Item -LiteralPath $sourceZlib -Destination (Join-Path $packageRoot 'zlib1.dll') -Force
Copy-Item -LiteralPath $sourceWeb -Destination (Join-Path $packageRoot 'assets\web') -Recurse -Force

$launchCmd = @'
@echo off
setlocal
cd /d "%~dp0"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File ".\Bootstrap-SunshineUpdate.ps1"
pause
'@
Set-Content -LiteralPath (Join-Path $packageRoot 'Launch-SunshineUpdate.cmd') -Value $launchCmd -Encoding ASCII

$bootstrapScript = @'
$ErrorActionPreference = 'Stop'

$sourceRoot = Split-Path -Parent $PSCommandPath
$runRoot = Join-Path $env:TEMP ('SunshineUpdate-Current-Run-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
$safeRoot = Join-Path $runRoot 'SunshineUpdate-Current'
$bootstrapLog = Join-Path $sourceRoot 'bootstrap.log'

function Write-BootstrapLog {
    param([string] $Message)
    $line = '{0} {1}' -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss.fff'), $Message
    Write-Host $line
    Add-Content -LiteralPath $bootstrapLog -Value $line
}

Write-BootstrapLog "Copying package to ASCII temp path: $safeRoot"
New-Item -ItemType Directory -Path $safeRoot -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $sourceRoot '*') -Destination $safeRoot -Recurse -Force

$installScript = Join-Path $safeRoot 'Install-SunshineUpdate.ps1'
Write-BootstrapLog "Launching elevated installer from temp path"
$process = Start-Process -FilePath 'powershell.exe' -Verb RunAs -Wait -PassThru -ArgumentList @(
    '-NoProfile',
    '-ExecutionPolicy', 'Bypass',
    '-File', "`"$installScript`""
)

Write-BootstrapLog "Elevated installer exit code: $($process.ExitCode)"
$tempInstallLog = Join-Path $safeRoot 'install.log'
if (Test-Path -LiteralPath $tempInstallLog) {
    Copy-Item -LiteralPath $tempInstallLog -Destination (Join-Path $sourceRoot 'install.log') -Force
    Write-BootstrapLog "Copied install.log back to original package folder"
}

if ($process.ExitCode -ne 0) {
    throw "Elevated installer failed with exit code $($process.ExitCode)"
}
'@
Set-Content -LiteralPath (Join-Path $packageRoot 'Bootstrap-SunshineUpdate.ps1') -Value $bootstrapScript -Encoding UTF8

$installScript = @'
$ErrorActionPreference = 'Stop'

$packageRoot = Split-Path -Parent $PSCommandPath
$logPath = Join-Path $packageRoot 'install.log'
$targetRoot = 'C:\Program Files\Sunshine'
$serviceName = 'SunshineService'

function Write-InstallLog {
    param([string] $Message)
    $line = '{0} {1}' -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss.fff'), $Message
    Write-Host $line
    Add-Content -LiteralPath $logPath -Value $line
}

$isAdmin = ([Security.Principal.WindowsPrincipal] [Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
    Start-Process -FilePath 'powershell.exe' -Verb RunAs -ArgumentList @(
        '-NoProfile',
        '-ExecutionPolicy', 'Bypass',
        '-File', "`"$PSCommandPath`""
    )
    exit
}

Write-InstallLog "Starting Sunshine update"
Write-InstallLog "Package: $packageRoot"
Write-InstallLog "Target: $targetRoot"

if (!(Test-Path -LiteralPath $targetRoot)) {
    throw "Target Sunshine directory not found: $targetRoot"
}

$required = @(
    'sunshine.exe',
    'tools\sunshine-wgc-helper.exe',
    'zlib1.dll',
    'assets\web'
)
foreach ($rel in $required) {
    $path = Join-Path $packageRoot $rel
    if (!(Test-Path -LiteralPath $path)) {
        throw "Required package file missing: $rel"
    }
}

$service = Get-Service -Name $serviceName -ErrorAction SilentlyContinue
if ($service -and $service.Status -ne 'Stopped') {
    Write-InstallLog "Stopping service $serviceName"
    Stop-Service -Name $serviceName -Force
    $service.WaitForStatus('Stopped', '00:00:30')
}

$processes = Get-Process sunshine -ErrorAction SilentlyContinue
if ($processes) {
    Write-InstallLog "Stopping remaining sunshine.exe process(es)"
    $processes | Stop-Process -Force
}

$backupRoot = Join-Path $targetRoot ('codex-backup-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
Write-InstallLog "Backing up old files to $backupRoot"
New-Item -ItemType Directory -Path $backupRoot | Out-Null

foreach ($rel in @('sunshine.exe', 'tools\sunshine-wgc-helper.exe', 'zlib1.dll', 'assets\web')) {
    $src = Join-Path $targetRoot $rel
    if (Test-Path -LiteralPath $src) {
        $dst = Join-Path $backupRoot $rel
        $dstParent = Split-Path -Parent $dst
        New-Item -ItemType Directory -Path $dstParent -Force | Out-Null
        Copy-Item -LiteralPath $src -Destination $dst -Recurse -Force
    }
}

Write-InstallLog "Copying updated files"
Copy-Item -LiteralPath (Join-Path $packageRoot 'sunshine.exe') -Destination (Join-Path $targetRoot 'sunshine.exe') -Force
New-Item -ItemType Directory -Path (Join-Path $targetRoot 'tools') -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $packageRoot 'tools\sunshine-wgc-helper.exe') -Destination (Join-Path $targetRoot 'tools\sunshine-wgc-helper.exe') -Force
Copy-Item -LiteralPath (Join-Path $packageRoot 'zlib1.dll') -Destination (Join-Path $targetRoot 'zlib1.dll') -Force

$targetWeb = Join-Path $targetRoot 'assets\web'
if (Test-Path -LiteralPath $targetWeb) {
    Remove-Item -LiteralPath $targetWeb -Recurse -Force
}
New-Item -ItemType Directory -Path (Split-Path -Parent $targetWeb) -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $packageRoot 'assets\web') -Destination $targetWeb -Recurse -Force

$configCandidates = @(
    (Join-Path $env:ProgramFiles 'Sunshine\config\sunshine.conf'),
    (Join-Path $env:ProgramData 'Sunshine\config\sunshine.conf'),
    (Join-Path $targetRoot 'config\sunshine.conf')
)
foreach ($conf in $configCandidates) {
    if (Test-Path -LiteralPath $conf) {
        $text = Get-Content -LiteralPath $conf -Raw
        if ($text -match '(?m)^\s*native_cursor\s*=') {
            $text = [regex]::Replace($text, '(?m)^\s*native_cursor\s*=.*$', 'native_cursor = enabled')
        } else {
            $text = $text.TrimEnd() + "`r`nnative_cursor = enabled`r`n"
        }
        Set-Content -LiteralPath $conf -Value $text -Encoding UTF8
        Write-InstallLog "Ensured native_cursor = enabled in $conf"
    }
}

if ($service) {
    Write-InstallLog "Starting service $serviceName"
    Start-Service -Name $serviceName
    (Get-Service -Name $serviceName).WaitForStatus('Running', '00:00:30')
    Write-InstallLog "Service status: $((Get-Service -Name $serviceName).Status)"
} else {
    Write-InstallLog "Service $serviceName not found; files were updated but service was not started"
}

$fileCount = (Get-ChildItem -LiteralPath $packageRoot -Recurse -File | Measure-Object).Count
$webCount = (Get-ChildItem -LiteralPath (Join-Path $packageRoot 'assets\web') -Recurse -File | Measure-Object).Count
Write-InstallLog "Package file count: $fileCount"
Write-InstallLog "Web UI file count: $webCount"
Write-InstallLog "Update complete"
'@
Set-Content -LiteralPath (Join-Path $packageRoot 'Install-SunshineUpdate.ps1') -Value $installScript -Encoding UTF8

$readme = @'
Sunshine target-machine update package.

Usage:
1. Copy this whole SunshineUpdate-Current folder to the target machine.
2. Double-click Launch-SunshineUpdate.cmd.
3. Approve UAC.

Launch-SunshineUpdate.cmd first copies the package to an ASCII-only temp directory, then runs the elevated installer from there. This avoids Windows command-line/code-page issues when the transfer folder contains Chinese characters.

The installer will:
- stop SunshineService and remaining sunshine.exe processes
- back up sunshine.exe, tools\sunshine-wgc-helper.exe, zlib1.dll, and assets\web
- copy the updated sunshine.exe, tools\sunshine-wgc-helper.exe, zlib1.dll, and full assets\web
- ensure native_cursor = enabled when a Sunshine config file is found
- restart SunshineService
- write install.log in this package directory

Default target directory: C:\Program Files\Sunshine
'@
Set-Content -LiteralPath (Join-Path $packageRoot 'README.txt') -Value $readme -Encoding UTF8

$shareText = @"
Sunshine update package:
$finalPackageRoot

Run on target machine:
Launch-SunshineUpdate.cmd
"@

New-Item -ItemType Directory -Path $transferRoot -Force | Out-Null
if (Test-Path -LiteralPath $finalPackageRoot) {
    Remove-Item -LiteralPath $finalPackageRoot -Recurse -Force
}
Copy-Item -LiteralPath $packageRoot -Destination $transferRoot -Recurse -Force
Set-Content -LiteralPath (Join-Path $transferRoot 'shareTXT.txt') -Value $shareText -Encoding UTF8

New-Item -ItemType Directory -Path $backupRoot -Force | Out-Null
$backupPackage = Join-Path $backupRoot ('SunshineUpdate-Current-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
Copy-Item -LiteralPath $packageRoot -Destination $backupPackage -Recurse -Force

$files = Get-ChildItem -LiteralPath $finalPackageRoot -Recurse -File
$totalBytes = ($files | Measure-Object -Property Length -Sum).Sum
$webFiles = Get-ChildItem -LiteralPath (Join-Path $finalPackageRoot 'assets\web') -Recurse -File

[pscustomobject]@{
    PackageRoot = $finalPackageRoot
    FileCount = $files.Count
    TotalMB = [math]::Round($totalBytes / 1MB, 2)
    WebFileCount = $webFiles.Count
    HasConfigFiles = [bool](Get-ChildItem -LiteralPath $finalPackageRoot -Recurse -File | Where-Object { $_.Name -match 'sunshine\.conf|sunshine\.log|\\.bak' })
    BackupRoot = $backupPackage
}
