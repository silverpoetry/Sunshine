# Local Sunshine Build/Update Notes

These notes are local machine procedures for Codex work on this checkout.

## Build

Do not invoke MSYS2 `gcc`, `g++`, `cmake`, or `ninja` directly from a plain PowerShell environment. The compiler can fail or pick the wrong runtime when the UCRT/MSYS PATH is incomplete.

Use:

```powershell
.\local-update-stage\Build-Sunshine-UCRT.ps1
```

Equivalent command:

```powershell
E:\Develop\MSYS2\msys64\usr\bin\bash.exe -lc "export PATH=/ucrt64/bin:/usr/bin:`$PATH; cd /d/Projects/sunshine && cmake -S . -B build-release-e-msys2 -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_DOCS=OFF && cmake --build build-release-e-msys2 --config Release -j 12"
```

Important build details:

- MSYS2 root: `E:\Develop\MSYS2\msys64`
- Build output: `D:\Projects\sunshine\build-release-e-msys2`
- Docs are disabled by default with `-DBUILD_DOCS=OFF`; this avoids requiring Doxygen/Graphviz in the normal optimized build.
- The build script clears only `build-release-e-msys2\.wix` before CMake configure. Upstream runs `dotnet tool install` for WiX during configure, and that command fails if the local `.wix` folder already exists.
- Version resources come from git during CMake configure. Always verify `sunshine.exe` `ProductVersion` after packaging if version correctness matters.

## Build Portable Package

For a GitHub-release-style portable folder, use CMake install rules instead of manually copying files:

```powershell
.\local-update-stage\Create-SunshinePortablePackage.ps1
```

Output:

- `C:\Users\weich\Desktop\穿梭机\SunshinePortable-x64`

The script installs into an ASCII-only temp staging directory first, then copies the completed portable folder to `穿梭机`. This avoids Unicode path issues in packaging tools.

Expected portable contents include:

- `sunshine.exe`
- `zlib1.dll`
- `tools\sunshine-wgc-helper.exe`
- `tools\sunshinesvc.exe`
- `tools\dxgi-info.exe`
- `tools\audio-info.exe`
- full `assets\web`

Do not include config files, logs, or backup files in the portable package.

## Update Local Portable Service

The local portable Sunshine service requires administrator rights to stop and replace files.

Use:

```powershell
.\local-update-stage\Update-LocalPortableSunshine.ps1
```

Important paths:

- Build output: `D:\Projects\sunshine\build-release-e-msys2`
- Portable root: `C:\Users\weich\Desktop\Tools\SystemTool\sunshine-windows-portable\Sunshine`
- Service name: `SunshineService`

The update flow must be:

1. Elevate to administrator.
2. Stop `SunshineService`.
3. Kill any remaining `sunshine.exe`.
4. Back up current runtime files.
5. Copy updated `sunshine.exe`, `tools\sunshine-wgc-helper.exe`, `zlib1.dll`, and full `assets\web`.
6. Start `SunshineService`.
7. Verify service status and copied file metadata.

## Convert Local Portable Service To Full MSI Install

The local machine can be migrated from the existing portable service root to a full Windows Installer install while keeping pairing identity and settings:

```powershell
.\local-update-stage\Install-LocalProgramFilesSunshine.ps1
```

Or double-click / run:

```cmd
.\local-update-stage\Install-LocalProgramFilesSunshine.cmd
```

Ready-to-run package folders:

- Project copy: `D:\Projects\sunshine\local-update-stage\files\SunshineMsiInstall-Current`
- Transfer copy: `C:\Users\weich\Desktop\穿梭机\SunshineMsiInstall-Current`

Each folder contains `SunshineInstaller-Current.msi`, `Install-LocalProgramFilesSunshine.cmd`, `Install-LocalProgramFilesSunshine.ps1`, and `README.txt`. The CMD prefers the MSI in the same folder, so it can be run from either location.

Current known source:

- Source root: auto-detected from `SunshineService` when possible, so either a portable root or an existing installed root is supported.
- Installed target: `C:\Program Files\Sunshine`
- MSI source: `D:\Projects\sunshine\build-release-e-msys2\cpack_artifacts\Sunshine.msi`

The script elevates, auto-detects the current Sunshine root from `SunshineService` when `-SourceRoot` is not provided, stops `SunshineService`, stops remaining `sunshine.exe`, removes stale `Sunshine` firewall rules, installs the MSI with `msiexec`, stops the newly started service, handles config preservation/migration, reapplies config ACLs, restarts the service, and verifies MSI registration, service path, process path, and certificate/private-key hashes.

No large directory backups are created by default. The script only writes small logs under `local-update-stage\migration-logs`.

Config behavior:

- If the source root is already `C:\Program Files\Sunshine`, the existing `config` directory is already correct and is preserved in place.
- If the source root is a portable directory, its active `config` identity files are copied into `C:\Program Files\Sunshine\config` after MSI installation.

If auto-detection is not appropriate, pass either name explicitly:

```powershell
.\local-update-stage\Install-LocalProgramFilesSunshine.ps1 -SourceRoot "C:\path\to\Sunshine"
.\local-update-stage\Install-LocalProgramFilesSunshine.ps1 -SourcePortableRoot "C:\path\to\Sunshine"
```

Use this route when the local machine must be indistinguishable from a normal Sunshine install in Windows Installer terms: uninstall registry, Start Menu shortcuts, MSI components, PATH, firewall, service install, and autostart all come from the MSI/custom actions.

The preserved identity/config files are:

- `config\sunshine.conf`
- `config\apps.json`
- `config\sunshine_state.json`
- `config\credentials\cacert.pem`
- `config\credentials\cakey.pem`
- optional `config\covers`
- optional `config\display_device.state`

Do not delete the old portable directory until the installed service has been tested from Moonlight.

## Build Target-Machine Update Package

For another Windows machine installed under `C:\Program Files\Sunshine`, build a self-contained update folder in Syncthing's transfer directory:

```powershell
.\local-update-stage\Create-TargetSunshineUpdatePackage.ps1
```

Output:

- `C:\Users\weich\Desktop\穿梭机\SunshineUpdate-Current`
- `C:\Users\weich\Desktop\穿梭机\shareTXT.txt`
- Local backup copy under `D:\Projects\sunshine\local-update-stage\package-backups\SunshineUpdate-Current-YYYYMMDD-HHMMSS`

Package contents:

- `sunshine.exe` from `build-release-e-msys2\sunshine.exe`
- `tools\sunshine-wgc-helper.exe` from `build-release-e-msys2\tools\sunshine-wgc-helper.exe`
- `zlib1.dll` from `E:\Develop\MSYS2\msys64\ucrt64\bin\zlib1.dll` when available
- full `assets\web` from `build-release-e-msys2\assets\web`
- `Launch-SunshineUpdate.cmd`
- `Bootstrap-SunshineUpdate.ps1`
- `Install-SunshineUpdate.ps1`
- `README.txt`

The launcher first copies the package to an ASCII-only temp directory, then requests administrator rights and runs the installer from there. This is intentional; Chinese transfer paths have caused update failures before.

The target installer must elevate, stop `SunshineService`, stop remaining `sunshine.exe`, back up old files, replace `sunshine.exe`, `tools\sunshine-wgc-helper.exe`, `zlib1.dll`, and full `assets\web`, ensure `native_cursor = enabled` if a config file is found, restart the service, and write `install.log`.

Do not include config files, logs, or backup files in the package.
