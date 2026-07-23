param(
    [string] $MsysRoot = 'E:\Develop\MSYS2\msys64',
    [string] $BuildName = 'build-release-e-msys2',
    [int] $Jobs = 12
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
$wixToolRoot = Join-Path $buildRoot '.wix'

if (!(Test-Path -LiteralPath $bash)) {
    throw "MSYS2 bash not found: $bash"
}

$msysRepo = Convert-ToMsysPath $repo
$msysBuild = Convert-ToMsysPath $buildRoot

# The upstream Windows CPack path runs `dotnet tool install` for WiX during
# configure. That command is not idempotent when the local .wix tool folder
# already exists, so clear just that generated tool cache before reconfigure.
if (Test-Path -LiteralPath $wixToolRoot) {
    Remove-Item -LiteralPath $wixToolRoot -Recurse -Force
}

$command = "export PATH=/ucrt64/bin:/usr/bin:`$PATH; cd '$msysRepo' && cmake -S . -B '$msysBuild' -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_DOCS=OFF && cmake --build '$msysBuild' --config Release -j $Jobs"

& $bash -lc $command
if ($LASTEXITCODE -ne 0) {
    throw "Sunshine build failed with exit code $LASTEXITCODE"
}

$required = @(
    (Join-Path $buildRoot 'sunshine.exe'),
    (Join-Path $buildRoot 'tools\sunshine-wgc-helper.exe'),
    (Join-Path $buildRoot 'tools\sunshinesvc.exe'),
    (Join-Path $buildRoot 'tools\dxgi-info.exe'),
    (Join-Path $buildRoot 'tools\audio-info.exe'),
    (Join-Path $buildRoot 'assets\web')
)

foreach ($path in $required) {
    if (!(Test-Path -LiteralPath $path)) {
        throw "Build completed but required output was not found: $path"
    }
}

$exe = Join-Path $buildRoot 'sunshine.exe'
$version = (Get-Item -LiteralPath $exe).VersionInfo.ProductVersion
[pscustomobject]@{
    BuildRoot = $buildRoot
    Version = $version
    Sunshine = $exe
    WebFiles = (Get-ChildItem -LiteralPath (Join-Path $buildRoot 'assets\web') -Recurse -File | Measure-Object).Count
}
