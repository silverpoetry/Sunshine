$ErrorActionPreference = 'Stop'

$repo = 'D:\Projects\sunshine'
$bash = 'C:\msys64\usr\bin\bash.exe'

if (!(Test-Path -LiteralPath $bash)) {
    throw "MSYS2 bash not found: $bash"
}

& $bash -lc "export PATH=/ucrt64/bin:/usr/bin:`$PATH; cd /d/Projects/sunshine && cmake --build build-release-local --config Release --target sunshine -j 12"
if ($LASTEXITCODE -ne 0) {
    throw "Sunshine build failed with exit code $LASTEXITCODE"
}

$exe = Join-Path $repo 'build-release-local\sunshine.exe'
if (!(Test-Path -LiteralPath $exe)) {
    throw "Build completed but sunshine.exe was not found: $exe"
}

Get-Item -LiteralPath $exe | Select-Object FullName, Length, LastWriteTime
