$ErrorActionPreference = 'Stop'

$repo = Split-Path -Parent $PSScriptRoot
$buildRoot = Join-Path $repo 'build-release-e-msys2'
$portableRoot = 'C:\Users\weich\Desktop\Tools\SystemTool\sunshine-windows-portable\Sunshine'
$serviceName = 'SunshineService'
$logPath = Join-Path $portableRoot 'local-update.log'
$msysZlib = 'E:\Develop\MSYS2\msys64\ucrt64\bin\zlib1.dll'

function Write-UpdateLog {
    param([string] $Message)
    $line = '{0} {1}' -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss.fff'), $Message
    Write-Host $line
    Add-Content -LiteralPath $logPath -Value $line
}

$isAdmin = ([Security.Principal.WindowsPrincipal] [Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
    $scriptPath = $PSCommandPath
    Start-Process -FilePath 'powershell.exe' -Verb RunAs -ArgumentList @(
        '-NoProfile',
        '-ExecutionPolicy', 'Bypass',
        '-File', "`"$scriptPath`""
    )
    exit
}

if (!(Test-Path -LiteralPath $portableRoot)) {
    throw "Portable root not found: $portableRoot"
}

$sourceFiles = @(
    @{ Source = Join-Path $buildRoot 'sunshine.exe'; Destination = Join-Path $portableRoot 'sunshine.exe' },
    @{ Source = Join-Path $buildRoot 'tools\sunshine-wgc-helper.exe'; Destination = Join-Path $portableRoot 'tools\sunshine-wgc-helper.exe' },
    @{ Source = $msysZlib; Destination = Join-Path $portableRoot 'zlib1.dll' }
)
$sourceWeb = Join-Path $buildRoot 'assets\web'
foreach ($entry in $sourceFiles) {
    if (!(Test-Path -LiteralPath $entry.Source)) {
        throw "Source file not found: $($entry.Source)"
    }
}
if (!(Test-Path -LiteralPath $sourceWeb)) {
    throw "Source Web UI not found: $sourceWeb"
}

Write-UpdateLog "Starting local portable Sunshine update"
Write-UpdateLog "Source build: $buildRoot"
Write-UpdateLog "Target: $portableRoot"

$service = Get-Service -Name $serviceName -ErrorAction SilentlyContinue
if ($service -and $service.Status -ne 'Stopped') {
    Write-UpdateLog "Stopping service $serviceName"
    Stop-Service -Name $serviceName -Force
    $service.WaitForStatus('Stopped', '00:00:20')
}

$processes = Get-Process sunshine -ErrorAction SilentlyContinue
if ($processes) {
    Write-UpdateLog "Stopping remaining sunshine.exe process(es)"
    $processes | Stop-Process -Force
}

$backupRoot = Join-Path $portableRoot ('codex-backup-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
Write-UpdateLog "Backing up current files to $backupRoot"
New-Item -ItemType Directory -Path $backupRoot | Out-Null

foreach ($rel in @('sunshine.exe', 'tools\sunshine-wgc-helper.exe', 'zlib1.dll', 'assets\web')) {
    $src = Join-Path $portableRoot $rel
    if (Test-Path -LiteralPath $src) {
        $dst = Join-Path $backupRoot $rel
        New-Item -ItemType Directory -Path (Split-Path -Parent $dst) -Force | Out-Null
        Copy-Item -LiteralPath $src -Destination $dst -Recurse -Force
    }
}

Write-UpdateLog "Copying updated executables and runtime files"
foreach ($entry in $sourceFiles) {
    New-Item -ItemType Directory -Path (Split-Path -Parent $entry.Destination) -Force | Out-Null
    Copy-Item -LiteralPath $entry.Source -Destination $entry.Destination -Force
}

$targetWeb = Join-Path $portableRoot 'assets\web'
if (Test-Path -LiteralPath $targetWeb) {
    Remove-Item -LiteralPath $targetWeb -Recurse -Force
}
New-Item -ItemType Directory -Path (Split-Path -Parent $targetWeb) -Force | Out-Null
Copy-Item -LiteralPath $sourceWeb -Destination $targetWeb -Recurse -Force

if ($service) {
    Write-UpdateLog "Starting service $serviceName"
    Start-Service -Name $serviceName
    (Get-Service -Name $serviceName).WaitForStatus('Running', '00:00:20')
}

$targetExe = Join-Path $portableRoot 'sunshine.exe'
$copied = Get-Item -LiteralPath $targetExe
Write-UpdateLog ("Updated exe length={0} lastWrite={1:o}" -f $copied.Length, $copied.LastWriteTime)
if ($service) {
    $current = Get-Service -Name $serviceName
    Write-UpdateLog "Service status: $($current.Status)"
}
Write-UpdateLog "Update complete"
