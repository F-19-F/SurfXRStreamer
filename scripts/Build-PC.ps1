[CmdletBinding()]
param([switch]$PrepareDependencies,[switch]$Package,[switch]$Setup,[string]$FrameArchive='',[switch]$CI)
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
$repositoryRoot = Split-Path -Parent $PSScriptRoot
$pcSource=Join-Path $repositoryRoot 'surfxr/pc/surfxr-server'
if (-not (Test-Path -LiteralPath (Join-Path $pcSource 'openvr/headers/openvr.h') -PathType Leaf)) {
    throw 'OpenVR SDK is missing. Run git submodule update --init --recursive from the repository root.'
}
$packageRequested = $Package -or $Setup
if ($packageRequested) {
    if (-not $FrameArchive) { throw 'Packaging requires -FrameArchive with a validated SurfXR Frame ZIP' }
    # Resolve relative arguments before changing into the Cargo workspace.
    $FrameArchive=(Resolve-Path -LiteralPath $FrameArchive).Path
}
$savedEnvironment = @{}
foreach ($name in @('LIBCLANG_PATH','SURFXR_FRAME_DESKTOP_DIR','SURFXR_FRAME_CLIENT_ZIP','CARGO_TARGET_DIR')) {
    $savedEnvironment[$name]=[Environment]::GetEnvironmentVariable($name, 'Process')
}
Push-Location $pcSource
try {
    $env:CARGO_TARGET_DIR=Join-Path $repositoryRoot 'build/cargo'
    if ($PrepareDependencies) {
        $prepareFlags = @('--platform','windows')
        if ($CI) { $prepareFlags += '--ci' }
        & cargo xtask prepare-deps @prepareFlags
        if ($LASTEXITCODE) { throw 'Dependency preparation failed' }
    }
    if (-not $env:LIBCLANG_PATH) { $env:LIBCLANG_PATH=Join-Path $pcSource 'deps/libclang/clang/native' }
    & (Join-Path $repositoryRoot 'scripts/build/build_surfxr_windows.ps1')
    $env:SURFXR_FRAME_DESKTOP_DIR=Join-Path $repositoryRoot 'build/pc/native/Release'
    & cargo xtask build-streamer --release
    if ($LASTEXITCODE) { throw 'SURFXR PC build failed' }
    if ($packageRequested) {
        $env:SURFXR_FRAME_CLIENT_ZIP=$FrameArchive
        $packageFlags=@('--portable','--no-rebuild')
        if ($Setup) { $packageFlags += '--setup' }
        & cargo xtask package-streamer @packageFlags
        if ($LASTEXITCODE) { throw 'SURFXR PC packaging failed' }
    }
} finally {
    Pop-Location
    foreach ($name in $savedEnvironment.Keys) {
        [Environment]::SetEnvironmentVariable($name, $savedEnvironment[$name], 'Process')
    }
}
