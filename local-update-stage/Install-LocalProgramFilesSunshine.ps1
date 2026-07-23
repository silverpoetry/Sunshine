param(
    [string] $MsiPath = 'D:\Projects\sunshine\build-release-e-msys2\cpack_artifacts\Sunshine.msi',
    [Alias('SourcePortableRoot')]
    [string] $SourceRoot = '',
    [string] $TargetRoot = 'C:\Program Files\Sunshine',
    [string] $ServiceName = 'SunshineService'
)

$ErrorActionPreference = 'Stop'

$repo = Split-Path -Parent $PSScriptRoot
$runRoot = Join-Path $repo 'local-update-stage\migration-logs'
$timestamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$runDir = Join-Path $runRoot "LocalMsiMigration-$timestamp"
$logPath = Join-Path $runDir 'install.log'
$msiLogPath = Join-Path $runDir 'msiexec.log'

function Write-InstallLog {
    param([string] $Message)

    $line = '{0} {1}' -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss.fff'), $Message
    Write-Host $line
    Add-Content -LiteralPath $logPath -Value $line
}

function Assert-SafeTargetRoot {
    param([string] $Path)

    $full = [System.IO.Path]::GetFullPath($Path).TrimEnd('\')
    $expected = [System.IO.Path]::GetFullPath('C:\Program Files\Sunshine').TrimEnd('\')
    if (![string]::Equals($full, $expected, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to install to unexpected target path: $full"
    }
}

function Get-NormalizedPath {
    param([string] $Path)

    return [System.IO.Path]::GetFullPath($Path).TrimEnd('\')
}

function Test-SamePath {
    param(
        [string] $Left,
        [string] $Right
    )

    return [string]::Equals((Get-NormalizedPath $Left), (Get-NormalizedPath $Right), [System.StringComparison]::OrdinalIgnoreCase)
}

function Copy-IfExists {
    param(
        [string] $Source,
        [string] $Destination
    )

    if (Test-Path -LiteralPath $Source) {
        New-Item -ItemType Directory -Path (Split-Path -Parent $Destination) -Force | Out-Null
        Copy-Item -LiteralPath $Source -Destination $Destination -Recurse -Force
        Write-InstallLog "Copied config item: $Source -> $Destination"
    }
}

function Write-TextUtf8NoBom {
    param(
        [string] $Path,
        [string] $Text
    )

    $encoding = New-Object System.Text.UTF8Encoding($false)
    [System.IO.File]::WriteAllText($Path, $Text, $encoding)
}

function Get-MsiProperty {
    param(
        [string] $Path,
        [string] $Property
    )

    $installer = New-Object -ComObject WindowsInstaller.Installer
    $database = $installer.GetType().InvokeMember('OpenDatabase', 'InvokeMethod', $null, $installer, @($Path, 0))
    $view = $database.GetType().InvokeMember('OpenView', 'InvokeMethod', $null, $database, @("SELECT `Value` FROM `Property` WHERE `Property`='$Property'"))
    $view.GetType().InvokeMember('Execute', 'InvokeMethod', $null, $view, $null)
    $record = $view.GetType().InvokeMember('Fetch', 'InvokeMethod', $null, $view, $null)
    if ($null -eq $record) {
        return $null
    }
    return $record.GetType().InvokeMember('StringData', 'GetProperty', $null, $record, 1)
}

function Stop-Sunshine {
    param([string] $Name)

    $service = Get-Service -Name $Name -ErrorAction SilentlyContinue
    if ($service -and $service.Status -ne 'Stopped') {
        Write-InstallLog "Stopping service $Name"
        Stop-Service -Name $Name -Force
        $service.WaitForStatus('Stopped', '00:00:30')
    }

    $processes = Get-Process sunshine -ErrorAction SilentlyContinue
    if ($processes) {
        Write-InstallLog "Stopping remaining sunshine.exe process(es)"
        $processes | Stop-Process -Force
    }
}

function Get-SunshineServices {
    Get-CimInstance Win32_Service |
        Where-Object { $_.Name -like '*Sunshine*' -or $_.DisplayName -like '*Sunshine*' } |
        Select-Object Name, DisplayName, State, StartMode, StartName, PathName
}

function Get-ServiceExecutablePath {
    param([string] $PathName)

    if (!$PathName) {
        return $null
    }
    if ($PathName -match '^\s*"([^"]+)"') {
        return $matches[1]
    }
    if ($PathName -match '^\s*([^\s]+)') {
        return $matches[1]
    }
    return $null
}

function Get-SunshineRootFromService {
    param([string] $Name)

    $service = Get-CimInstance Win32_Service -Filter "Name='$Name'" -ErrorAction SilentlyContinue
    if (!$service) {
        return $null
    }

    $exePath = Get-ServiceExecutablePath -PathName $service.PathName
    if (!$exePath) {
        return $null
    }

    $leaf = Split-Path -Leaf $exePath
    $parent = Split-Path -Parent $exePath
    if ([string]::Equals($leaf, 'sunshinesvc.exe', [System.StringComparison]::OrdinalIgnoreCase)) {
        if ([string]::Equals((Split-Path -Leaf $parent), 'tools', [System.StringComparison]::OrdinalIgnoreCase)) {
            return Split-Path -Parent $parent
        }
    }
    if ([string]::Equals($leaf, 'sunshine.exe', [System.StringComparison]::OrdinalIgnoreCase)) {
        return $parent
    }

    return $null
}

function Test-SunshineConfigRoot {
    param([string] $Root)

    if (!$Root) {
        return $false
    }
    $configRoot = Join-Path $Root 'config'
    foreach ($rel in @('sunshine.conf', 'apps.json', 'sunshine_state.json', 'credentials\cacert.pem', 'credentials\cakey.pem')) {
        if (!(Test-Path -LiteralPath (Join-Path $configRoot $rel))) {
            return $false
        }
    }
    return $true
}

function Resolve-SourceRoot {
    param(
        [string] $RequestedRoot,
        [string] $InstallTargetRoot,
        [string] $Name
    )

    if (![string]::IsNullOrWhiteSpace($RequestedRoot)) {
        return Get-NormalizedPath $RequestedRoot
    }

    $serviceRoot = Get-SunshineRootFromService -Name $Name
    if (Test-SunshineConfigRoot -Root $serviceRoot) {
        return Get-NormalizedPath $serviceRoot
    }

    $fallbackRoots = @(
        'C:\Users\weich\Desktop\Tools\SystemTool\sunshine-windows-portable\Sunshine',
        $InstallTargetRoot
    )
    foreach ($root in $fallbackRoots) {
        if (Test-SunshineConfigRoot -Root $root) {
            return Get-NormalizedPath $root
        }
    }

    throw "Could not auto-detect an existing Sunshine config root. Pass -SourceRoot or -SourcePortableRoot explicitly."
}

$isAdmin = ([Security.Principal.WindowsPrincipal] [Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
    $process = Start-Process -FilePath 'powershell.exe' -Verb RunAs -Wait -PassThru -ArgumentList @(
        '-NoProfile',
        '-ExecutionPolicy', 'Bypass',
        '-File', "`"$PSCommandPath`"",
        '-MsiPath', "`"$MsiPath`"",
        '-SourceRoot', "`"$SourceRoot`"",
        '-TargetRoot', "`"$TargetRoot`"",
        '-ServiceName', "`"$ServiceName`""
    )
    exit $process.ExitCode
}

Assert-SafeTargetRoot $TargetRoot
$SourceRoot = Resolve-SourceRoot -RequestedRoot $SourceRoot -InstallTargetRoot $TargetRoot -Name $ServiceName

New-Item -ItemType Directory -Path $runDir -Force | Out-Null
Write-InstallLog "Starting local MSI migration while preserving portable identity"
Write-InstallLog "MSI: $MsiPath"
Write-InstallLog "Source root: $SourceRoot"
Write-InstallLog "Target root: $TargetRoot"
Write-InstallLog "Service name: $ServiceName"

if (!(Test-Path -LiteralPath $MsiPath)) {
    throw "MSI not found: $MsiPath"
}

$sourceConfig = Join-Path $SourceRoot 'config'
$targetConfig = Join-Path $TargetRoot 'config'
$sameConfigRoot = Test-SamePath -Left $sourceConfig -Right $targetConfig
foreach ($rel in @('sunshine.conf', 'apps.json', 'sunshine_state.json', 'credentials\cacert.pem', 'credentials\cakey.pem')) {
    $path = Join-Path $sourceConfig $rel
    if (!(Test-Path -LiteralPath $path)) {
        throw "Source config is missing required identity/config item: $path"
    }
}
$sourceCertHashBefore = (Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $sourceConfig 'credentials\cacert.pem')).Hash
$sourceKeyHashBefore = (Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $sourceConfig 'credentials\cakey.pem')).Hash
Write-InstallLog "Source config: $sourceConfig"
Write-InstallLog "Target config: $targetConfig"
Write-InstallLog "Source config is target config: $sameConfigRoot"

$msiProductName = Get-MsiProperty -Path $MsiPath -Property 'ProductName'
$msiProductVersion = Get-MsiProperty -Path $MsiPath -Property 'ProductVersion'
$msiProductCode = Get-MsiProperty -Path $MsiPath -Property 'ProductCode'
$msiUpgradeCode = Get-MsiProperty -Path $MsiPath -Property 'UpgradeCode'
Write-InstallLog "MSI ProductName: $msiProductName"
Write-InstallLog "MSI ProductVersion: $msiProductVersion"
Write-InstallLog "MSI ProductCode: $msiProductCode"
Write-InstallLog "MSI UpgradeCode: $msiUpgradeCode"

Write-InstallLog "Sunshine services before install:"
Get-SunshineServices | ForEach-Object { Write-InstallLog ("  {0} state={1} start={2} path={3}" -f $_.Name, $_.State, $_.StartMode, $_.PathName) }

Stop-Sunshine -Name $ServiceName

Write-InstallLog "Skipping large directory backups by design"

Write-InstallLog "Removing stale Sunshine firewall rules before MSI install"
& netsh advfirewall firewall delete rule name=Sunshine | ForEach-Object { Write-InstallLog $_ }

Write-InstallLog "Installing MSI through Windows Installer"
$msiArgs = @(
    '/i', "`"$MsiPath`"",
    '/qn',
    '/norestart',
    '/L*v', "`"$msiLogPath`""
)
$msiProcess = Start-Process -FilePath 'msiexec.exe' -ArgumentList $msiArgs -Wait -PassThru
Write-InstallLog "msiexec exit code: $($msiProcess.ExitCode)"
if ($msiProcess.ExitCode -notin @(0, 3010)) {
    throw "MSI install failed with exit code $($msiProcess.ExitCode). See $msiLogPath"
}

Stop-Sunshine -Name $ServiceName

if ($sameConfigRoot) {
    Write-InstallLog "Existing install already uses the target config directory; preserving config in place"
} else {
    if (Test-Path -LiteralPath $targetConfig) {
        Write-InstallLog "Removing MSI-created target config before restoring source identity"
        Remove-Item -LiteralPath $targetConfig -Recurse -Force
    }
    New-Item -ItemType Directory -Path $targetConfig -Force | Out-Null

    Write-InstallLog "Restoring source config, client pairing state, and certificate keys"
    Copy-IfExists -Source (Join-Path $sourceConfig 'sunshine.conf') -Destination (Join-Path $targetConfig 'sunshine.conf')
    Copy-IfExists -Source (Join-Path $sourceConfig 'apps.json') -Destination (Join-Path $targetConfig 'apps.json')
    Copy-IfExists -Source (Join-Path $sourceConfig 'sunshine_state.json') -Destination (Join-Path $targetConfig 'sunshine_state.json')
    Copy-IfExists -Source (Join-Path $sourceConfig 'credentials') -Destination (Join-Path $targetConfig 'credentials')
    Copy-IfExists -Source (Join-Path $sourceConfig 'covers') -Destination (Join-Path $targetConfig 'covers')
    Copy-IfExists -Source (Join-Path $sourceConfig 'display_device.state') -Destination (Join-Path $targetConfig 'display_device.state')
}

$targetConf = Join-Path $targetConfig 'sunshine.conf'
$confText = Get-Content -LiteralPath $targetConf -Raw
if ($confText -notmatch '(?m)^\s*native_cursor\s*=\s*enabled\s*$') {
    if ($confText -match '(?m)^\s*native_cursor\s*=') {
        $confText = [regex]::Replace($confText, '(?m)^\s*native_cursor\s*=.*$', 'native_cursor = enabled')
    } else {
        $confText = $confText.TrimEnd() + "`r`nnative_cursor = enabled`r`n"
    }
    Write-TextUtf8NoBom -Path $targetConf -Text $confText
    Write-InstallLog "Ensured native_cursor = enabled"
} else {
    Write-InstallLog "native_cursor already enabled; sunshine.conf left unchanged"
}

Write-InstallLog "Applying installed-config ACLs"
& icacls.exe $targetConfig /reset | ForEach-Object { Write-InstallLog $_ }
$targetCredentials = Join-Path $targetConfig 'credentials'
if (Test-Path -LiteralPath $targetCredentials) {
    & icacls.exe $targetCredentials /inheritance:r | ForEach-Object { Write-InstallLog $_ }
    & icacls.exe $targetCredentials /grant:r '*S-1-5-32-544:(OI)(CI)(F)' | ForEach-Object { Write-InstallLog $_ }
    & icacls.exe $targetCredentials /grant:r '*S-1-5-32-545:(R)' | ForEach-Object { Write-InstallLog $_ }
}

Write-InstallLog "Starting installed SunshineService"
Start-Service -Name $ServiceName
(Get-Service -Name $ServiceName).WaitForStatus('Running', '00:00:30')
Start-Sleep -Seconds 3

$serviceInfo = Get-CimInstance Win32_Service -Filter "Name='$ServiceName'"
$sunshineProcess = Get-CimInstance Win32_Process -Filter "Name='sunshine.exe'" | Select-Object -First 1
$targetCertHash = (Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $targetConfig 'credentials\cacert.pem')).Hash
$targetKeyHash = (Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $targetConfig 'credentials\cakey.pem')).Hash
$version = (Get-Item -LiteralPath (Join-Path $TargetRoot 'sunshine.exe')).VersionInfo.ProductVersion
$uninstallKeys = @(
    'HKLM:\Software\Microsoft\Windows\CurrentVersion\Uninstall\*',
    'HKLM:\Software\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\*'
)
$registered = Get-ItemProperty -Path $uninstallKeys -ErrorAction SilentlyContinue |
    Where-Object { $_.DisplayName -eq 'Sunshine' } |
    Select-Object -First 1
$sunshineServices = @(Get-SunshineServices)
$unexpectedServices = @($sunshineServices | Where-Object { $_.Name -ne $ServiceName })

Write-InstallLog "Sunshine services after install:"
$sunshineServices | ForEach-Object { Write-InstallLog ("  {0} state={1} start={2} path={3}" -f $_.Name, $_.State, $_.StartMode, $_.PathName) }
Write-InstallLog "Service path: $($serviceInfo.PathName)"
Write-InstallLog "Service state: $($serviceInfo.State)"
Write-InstallLog "Running sunshine.exe: $(if ($sunshineProcess) { $sunshineProcess.ExecutablePath } else { '<not observed>' })"
Write-InstallLog "Sunshine file version: $version"
Write-InstallLog "MSI uninstall registration present: $([bool] $registered)"
Write-InstallLog "Certificate hash preserved: $($sourceCertHashBefore -eq $targetCertHash)"
Write-InstallLog "Private key hash preserved: $($sourceKeyHashBefore -eq $targetKeyHash)"
Write-InstallLog "Log directory: $runDir"
Write-InstallLog "MSI log: $msiLogPath"
if ($unexpectedServices.Count -gt 0) {
    throw "Unexpected extra Sunshine service(s) found after install: $($unexpectedServices.Name -join ', ')"
}
Write-InstallLog "Migration complete"

[pscustomobject]@{
    TargetRoot = $TargetRoot
    FileVersion = $version
    MsiProductVersion = $msiProductVersion
    ServiceState = $serviceInfo.State
    ServicePath = $serviceInfo.PathName
    RunningExe = if ($sunshineProcess) { $sunshineProcess.ExecutablePath } else { '<not observed>' }
    MsiRegistered = [bool] $registered
    CertificatePreserved = ($sourceCertHashBefore -eq $targetCertHash)
    PrivateKeyPreserved = ($sourceKeyHashBefore -eq $targetKeyHash)
    LogDirectory = $runDir
    LogPath = $logPath
    MsiLogPath = $msiLogPath
}
