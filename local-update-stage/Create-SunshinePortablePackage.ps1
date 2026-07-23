param(
    [string] $MsysRoot = 'E:\Develop\MSYS2\msys64',
    [string] $BuildName = 'build-release-e-msys2',
    [string] $PackageName = 'SunshinePortable-x64'
)

$ErrorActionPreference = 'Stop'

function Convert-ToMsysPath {
    param([Parameter(Mandatory = $true)][string] $Path)

    $full = [System.IO.Path]::GetFullPath($Path)
    $drive = $full.Substring(0, 1).ToLowerInvariant()
    $rest = $full.Substring(2).Replace('\', '/')
    return '/' + $drive + $rest
}

$repo = Split-Path -Parent $PSScriptRoot
$bash = Join-Path $MsysRoot 'usr\bin\bash.exe'
$buildRoot = Join-Path $repo $BuildName
$transferFolderName = -join @([char]0x7a7f, [char]0x68ad, [char]0x673a)
$transferRoot = Join-Path (Join-Path $env:USERPROFILE 'Desktop') $transferFolderName
$finalPackageRoot = Join-Path $transferRoot $PackageName
$stagingRoot = Join-Path $env:TEMP ($PackageName + '-Staging')
$stagingPackageRoot = Join-Path $stagingRoot $PackageName

if (!(Test-Path -LiteralPath $bash)) {
    throw "MSYS2 bash not found: $bash"
}
if (!(Test-Path -LiteralPath (Join-Path $buildRoot 'sunshine.exe'))) {
    throw "Build output not found. Run local-update-stage\Build-Sunshine-UCRT.ps1 first: $buildRoot"
}

if (Test-Path -LiteralPath $stagingRoot) {
    Remove-Item -LiteralPath $stagingRoot -Recurse -Force
}
New-Item -ItemType Directory -Path $stagingPackageRoot | Out-Null

$msysRepo = Convert-ToMsysPath $repo
$msysBuild = Convert-ToMsysPath $buildRoot
$msysPrefix = Convert-ToMsysPath $stagingPackageRoot
$command = "export PATH=/ucrt64/bin:/usr/bin:`$PATH; cd '$msysRepo' && cmake --install '$msysBuild' --config Release --prefix '$msysPrefix' --strip"

& $bash -lc $command
if ($LASTEXITCODE -ne 0) {
    throw "Sunshine install/package failed with exit code $LASTEXITCODE"
}

$required = @(
    'sunshine.exe',
    'zlib1.dll',
    'tools\sunshine-wgc-helper.exe',
    'tools\sunshinesvc.exe',
    'tools\dxgi-info.exe',
    'tools\audio-info.exe',
    'assets\web'
)
foreach ($rel in $required) {
    $path = Join-Path $stagingPackageRoot $rel
    if (!(Test-Path -LiteralPath $path)) {
        throw "Packaged output missing required file: $rel"
    }
}

New-Item -ItemType Directory -Path $transferRoot -Force | Out-Null
if (Test-Path -LiteralPath $finalPackageRoot) {
    Remove-Item -LiteralPath $finalPackageRoot -Recurse -Force
}
Copy-Item -LiteralPath $stagingPackageRoot -Destination $transferRoot -Recurse -Force

$files = Get-ChildItem -LiteralPath $finalPackageRoot -Recurse -File
$totalBytes = ($files | Measure-Object -Property Length -Sum).Sum
$webFiles = Get-ChildItem -LiteralPath (Join-Path $finalPackageRoot 'assets\web') -Recurse -File
$version = (Get-Item -LiteralPath (Join-Path $finalPackageRoot 'sunshine.exe')).VersionInfo.ProductVersion
$unwanted = Get-ChildItem -LiteralPath $finalPackageRoot -Recurse -File |
    Where-Object { $_.Name -match 'sunshine\.conf|sunshine\.log|\.bak|codex-backup' }

[pscustomobject]@{
    PackageRoot = $finalPackageRoot
    Version = $version
    FileCount = $files.Count
    TotalMB = [math]::Round($totalBytes / 1MB, 2)
    WebFileCount = $webFiles.Count
    HasUnwantedRuntimeFiles = [bool] $unwanted
}
