# Local Sunshine Build/Update Notes

These notes are local machine procedures for Codex work on this checkout.

## Build

Do not invoke MSYS2 `gcc`, `g++`, `cmake`, or `ninja` directly from a plain PowerShell environment. The compiler can fail silently because the UCRT/MSYS runtime PATH is incomplete.

Use:

```powershell
.\local-update-stage\Build-Sunshine-UCRT.ps1
```

Equivalent command:

```powershell
C:\msys64\usr\bin\bash.exe -lc "export PATH=/ucrt64/bin:/usr/bin:`$PATH; cd /d/Projects/sunshine && cmake --build build-release-local --config Release --target sunshine -j 12"
```

## Update Local Portable Service

The local portable Sunshine service requires administrator rights to stop and replace files.

Use:

```powershell
.\local-update-stage\Update-LocalPortableSunshine.ps1
```

Important paths:

- Build output: `D:\Projects\sunshine\build-release-local\sunshine.exe`
- Portable root: `C:\Users\weich\Desktop\Tools\SystemTool\sunshine-windows-portable\Sunshine`
- Service name: `SunshineService`

The update flow must be:

1. Elevate to administrator.
2. Stop `SunshineService`.
3. Kill any remaining `sunshine.exe`.
4. Backup existing `sunshine.exe`.
5. Copy the new `sunshine.exe`.
6. Start `SunshineService`.
7. Verify service status and copied file metadata.

## Build Target-Machine Update Package

For another Windows machine installed under `C:\Program Files\Sunshine`, build a self-contained update folder in Syncthing's transfer directory:

```powershell
.\local-update-stage\Create-TargetSunshineUpdatePackage.ps1
```

Output:

- `C:\Users\weich\Desktop\穿梭机\SunshineUpdate-Current`
- `C:\Users\weich\Desktop\穿梭机\shareTXT.txt`
- Local backup copies should be kept under `D:\Projects\sunshine\local-update-stage\package-backups\SunshineUpdate-Current-YYYYMMDD-HHMMSS`

Package contents:

- `sunshine.exe` from `build-release-local\sunshine.exe`
- `tools\sunshine-wgc-helper.exe` from `build-release-local\tools\sunshine-wgc-helper.exe`
- `zlib1.dll` from the current local portable Sunshine folder
- full `assets\web` from `build-release-local\assets\web`
- `Launch-SunshineUpdate.cmd`
- `Install-SunshineUpdate.ps1`
- `README.txt`

The target installer must elevate, stop `SunshineService`, stop remaining `sunshine.exe`, back up old files, replace `sunshine.exe`, `tools\sunshine-wgc-helper.exe`, `zlib1.dll`, and full `assets\web`, ensure `native_cursor = enabled` if a config file is found, restart the service, and write `install.log`.

Do not include config files, logs, or backup files in the package.

Chinese path handling note: generate the package in an ASCII-only staging directory first, then copy the completed folder to `穿梭机`. Keep the generator script ASCII-only for Windows PowerShell 5.1 compatibility.
