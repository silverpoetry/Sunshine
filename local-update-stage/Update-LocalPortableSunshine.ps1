$ErrorActionPreference = 'Stop'

$sourceExe = 'D:\Projects\sunshine\build-release-local\sunshine.exe'
$portableRoot = 'C:\Users\weich\Desktop\Tools\SystemTool\sunshine-windows-portable\Sunshine'
$serviceName = 'SunshineService'
$logPath = Join-Path $portableRoot 'local-update.log'

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

if (!(Test-Path -LiteralPath $sourceExe)) {
    throw "Source executable not found: $sourceExe"
}
if (!(Test-Path -LiteralPath $portableRoot)) {
    throw "Portable root not found: $portableRoot"
}

Write-UpdateLog "Starting local portable Sunshine update"
Write-UpdateLog "Source: $sourceExe"
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

$targetExe = Join-Path $portableRoot 'sunshine.exe'
$backupExe = Join-Path $portableRoot ('sunshine.exe.bak-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
if (Test-Path -LiteralPath $targetExe) {
    Write-UpdateLog "Backing up current exe to $backupExe"
    Copy-Item -LiteralPath $targetExe -Destination $backupExe -Force
}

Write-UpdateLog "Copying new sunshine.exe"
Copy-Item -LiteralPath $sourceExe -Destination $targetExe -Force

if ($service) {
    Write-UpdateLog "Starting service $serviceName"
    Start-Service -Name $serviceName
    (Get-Service -Name $serviceName).WaitForStatus('Running', '00:00:20')
}

$copied = Get-Item -LiteralPath $targetExe
Write-UpdateLog ("Updated exe length={0} lastWrite={1:o}" -f $copied.Length, $copied.LastWriteTime)
if ($service) {
    $current = Get-Service -Name $serviceName
    Write-UpdateLog "Service status: $($current.Status)"
}
Write-UpdateLog "Update complete"
