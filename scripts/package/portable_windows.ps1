# Private staging step of cargo xtask package-streamer --portable.
# Inputs are already built payloads. No pairing, service installation or downloads.
param([Parameter(Mandatory=$true)][string]$Source, [switch]$Setup)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression.FileSystem
# Use .NET directly so Windows PowerShell and pwsh module paths need not match.
function Get-PayloadHash([string]$Path) {
    $stream = [IO.File]::OpenRead($Path)
    $algorithm = [Security.Cryptography.SHA256]::Create()
    try { return [BitConverter]::ToString($algorithm.ComputeHash($stream)).Replace('-','').ToLowerInvariant() }
    finally { $algorithm.Dispose(); $stream.Dispose() }
}
$sourceRoot = (Resolve-Path -LiteralPath $Source).Path
$crtRoot = (Resolve-Path -LiteralPath $env:SURFXR_MSVC_REDIST_DIR).Path
$frameZip = (Resolve-Path -LiteralPath $env:SURFXR_FRAME_CLIENT_ZIP).Path
$releaseId = $env:SURFXR_RELEASE_ID
if (!$releaseId) { $releaseId = '1.0.0' }
if ($releaseId -notmatch '^[A-Za-z0-9._+-]{1,64}$') { throw 'Invalid SURFXR_RELEASE_ID' }
if ($Setup -and !(Test-Path -LiteralPath $env:SURFXR_MAKENSIS -PathType Leaf)) {
    throw 'Set SURFXR_MAKENSIS to the NSIS 3 Unicode makensis.exe compiler'
}
$installerScripts = @('frame-installer/Install-Frame.ps1','frame-installer/launch/Start-Frame.ps1',
    'frame-installer/launch/frame_release.ps1','frame-installer/launch/frame_release_commands.ps1')
foreach ($required in (@('SurfXR Dashboard.exe','bin/win64/driver_surfxr_server.dll','surfxr-daemon.exe',
    'surfxr-desktop.exe','surfxr-desktop-module.dll','bin/win64/surfxr_frame_worker.exe',
    'licenses/dependencies.html') + $installerScripts)) {
    if (!(Test-Path -LiteralPath (Join-Path $sourceRoot $required) -PathType Leaf)) {
        throw "Required release payload missing: $required"
    }
}
foreach ($required in @('msvcp140.dll','vcruntime140.dll','vcruntime140_1.dll')) {
    if (!(Test-Path -LiteralPath (Join-Path $crtRoot $required) -PathType Leaf)) {
        throw "Required Microsoft x64 redistributable missing: $required"
    }
}
$repositoryRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$buildRoot = Join-Path $repositoryRoot 'build/package/windows'
$releaseRoot = Join-Path $repositoryRoot 'release'
New-Item -ItemType Directory -Path $buildRoot,$releaseRoot -Force | Out-Null
$stage = Join-Path $buildRoot ('portable-' + [guid]::NewGuid().ToString('N'))
$folder = Join-Path $stage 'SurfXR'
New-Item -ItemType Directory -Path $folder -Force | Out-Null
foreach ($notice in @('LICENSE','NOTICE.md')) {
    Copy-Item -LiteralPath (Join-Path $repositoryRoot $notice) -Destination $folder
}
# Allowlist avoids carrying live session files, client trust, logs or debug symbols.
Get-ChildItem -LiteralPath $sourceRoot -File -Recurse | ForEach-Object {
    $relative = $_.FullName.Substring($sourceRoot.Length + 1)
    if ($_.Extension -in @('.exe','.dll','.vrdrivermanifest') -or
        $relative.StartsWith('licenses\') -or $relative -eq 'FFmpeg.LICENSE.txt' -or $relative.Replace('\','/') -in $installerScripts) {
        $destination = Join-Path $folder $relative
        New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force | Out-Null
        Copy-Item -LiteralPath $_.FullName -Destination $destination
    }
}
$crtFiles = @(Get-ChildItem -LiteralPath $crtRoot -Filter '*.dll' -File)
foreach ($file in $crtFiles) {
    Copy-Item -LiteralPath $file.FullName -Destination $folder
    Copy-Item -LiteralPath $file.FullName -Destination (Join-Path $folder 'bin/win64')
}
# The worker is a separate executable; keep its native dependencies app-local too.
Get-ChildItem -LiteralPath $folder -Filter '*.dll' -File | ForEach-Object {
    Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $folder 'bin/win64')
}
$frameStage = Join-Path $stage 'frame-extracted'
# The Archive module inherited from pwsh may require .NET overloads absent in PS 5.1.
[IO.Compression.ZipFile]::ExtractToDirectory($frameZip, $frameStage)
$frameRoots = @(Get-ChildItem -LiteralPath $frameStage -Directory)
if ($frameRoots.Count -ne 1) { throw 'Expected one Frame release root' }
$frameSource = $frameRoots[0].FullName
$frameManifest = Get-Content -LiteralPath (Join-Path $frameSource 'release-manifest.json') -Raw | ConvertFrom-Json
if ($frameManifest.format_version -ne 2) { throw 'Expected native Frame release format v2' }
if ($frameManifest.install_profile -ne 'surfxr') {
    throw 'SurfXR requires a Frame archive built with --release-profile surfxr; the selected archive has an incompatible profile.'
}
foreach ($entry in $frameManifest.files.PSObject.Properties) {
    $entryPath = [IO.Path]::GetFullPath((Join-Path $frameSource $entry.Name))
    if (!$entryPath.StartsWith($frameSource + [IO.Path]::DirectorySeparatorChar,
        [StringComparison]::OrdinalIgnoreCase)) { throw 'Invalid Frame manifest path' }
    if ((Get-PayloadHash $entryPath) -ne $entry.Value) {
        throw "Frame release hash mismatch: $($entry.Name)"
    }
}
Copy-Item -LiteralPath $frameSource -Destination (Join-Path $folder 'frame-client') -Recurse
$utf8 = New-Object System.Text.UTF8Encoding($false)
[IO.File]::WriteAllText((Join-Path $folder 'Install-SteamFrame.cmd'),
    "@echo off`r`ncall `"%~dp0frame-client\Install-Frame.cmd`" %*`r`n", $utf8)
[IO.File]::WriteAllText((Join-Path $folder 'README.txt'), @'
SurfXR - Windows 10/11 x64 portable

1. Extract the entire folder and open SurfXR Dashboard.exe.
2. Install/update the headset with Install-SteamFrame.cmd. SSH/Python are bundled.
   This installs SurfXR on the headset for the SurfXR PC server.
   Trust the Frame device in SurfXR Dashboard.
3. Steam and SteamVR must already be installed. GPU drivers are supplied by your GPU vendor.
4. In Dashboard > Desktop, install/start the streaming service when needed (administrator prompt).

Microphone: Steam Streaming Microphone is the default Windows route. Choose it as the
input in your game or voice app. Settings > Advanced > Audio contains device selection
and Disable; no separate Virtual Audio Cable installation is needed for the Steam route.

Quick setup resolution uses SurfXR per-eye tiers; default is Godlike, 3072 x 3264 per eye.
Devices > New Wireless Devices > One-time pairing trusts one new device, then turns off once it connects.
It is off on each Dashboard launch; paired devices remain in the trusted device list.
Choose English or Simplified Chinese in the sidebar or first-run setup wizard.
Language preferences are saved separately from streaming settings.
Your saved settings are preserved when updating an existing installation.
This package contains no account credentials, trusted clients or personal session settings.
Microsoft CRT DLLs are bundled app-locally. Steam, SteamVR, audio and GPU drivers are not bundled.
'@, $utf8)
[IO.File]::WriteAllText((Join-Path $folder 'MSVC-REDIST.txt'), @"
Microsoft Visual C++ x64 Runtime $($crtFiles[0].VersionInfo.FileVersion)
Copyright Microsoft Corporation. Unmodified app-local Microsoft.VC143.CRT redistributables.
Redistribution list: https://learn.microsoft.com/en-us/visualstudio/releases/2022/redistribution
License terms: https://visualstudio.microsoft.com/license-terms/
"@, $utf8)
$hashes = [ordered]@{}
Get-ChildItem -LiteralPath $folder -Recurse -File | Sort-Object FullName | ForEach-Object {
    $hashes[$_.FullName.Substring($folder.Length+1).Replace('\','/')] =
        (Get-PayloadHash $_.FullName)
}
$revision = & git -C (Join-Path $PSScriptRoot '../..') rev-parse HEAD
if ($LASTEXITCODE -ne 0) { throw 'Cannot read source revision' }
$manifest = [ordered]@{format_version=1; release_id=$releaseId; source_commit=$revision; architecture='x64';
    frame_source_commit=$frameManifest.source_commit; private_configuration_included=$false;
    frame_archive_sha256=(Get-PayloadHash $frameZip); files=$hashes}
[IO.File]::WriteAllText((Join-Path $folder 'pc-release-manifest.json'),
    ($manifest | ConvertTo-Json -Depth 5), $utf8)
$archive = Join-Path $releaseRoot 'SurfXR-Windows-portable.zip'
if (Test-Path -LiteralPath $archive) { Remove-Item -LiteralPath $archive }
[IO.Compression.ZipFile]::CreateFromDirectory($folder, $archive,
    [IO.Compression.CompressionLevel]::Optimal, $true)
[IO.File]::WriteAllText(($archive + '.sha256'),
    ((Get-PayloadHash $archive) + "`n"), $utf8)
Write-Output "Portable package: $archive"
Write-Output "Staged payload: $folder"
if ($Setup) {
    & (Join-Path $PSScriptRoot 'setup_windows.ps1') -Payload $folder -OutputDirectory $releaseRoot
}
