# Isolated production-packager fixture. Never produces a usable installation.
param([switch]$CompileSetup)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
Add-Type -AssemblyName System.IO.Compression.FileSystem
$repository=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$parent=Join-Path $repository 'build/tests'
$fixture=Join-Path $parent ('portable-adapter-'+[Guid]::NewGuid().ToString('N'))
$saved=@{}
foreach($name in @('SURFXR_MSVC_REDIST_DIR','SURFXR_FRAME_CLIENT_ZIP','SURFXR_RELEASE_ID')) {
    $saved[$name]=[Environment]::GetEnvironmentVariable($name,'Process')
}
function Write-Fixture([string]$Path,[string]$Text) {
    New-Item -ItemType Directory -Path (Split-Path $Path) -Force | Out-Null
    [IO.File]::WriteAllText($Path,$Text)
}
function Assert-Check([bool]$Value,[string]$Message) {if(!$Value){throw "Portable fixture failed: $Message"}}
try {
    $workspace=Join-Path $fixture 'repo'
    $script=Join-Path $workspace 'scripts/package/portable_windows.ps1'
    Write-Fixture $script ''
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'portable_windows.ps1') -Destination $script
    foreach($name in @('setup_windows.ps1','setup_windows.nsi')) {
        Copy-Item -LiteralPath (Join-Path $PSScriptRoot $name) -Destination (Split-Path $script)
    }
    foreach($name in @('LICENSE','NOTICE.md')) {Write-Fixture (Join-Path $workspace $name) 'fixture notice'}
    $source=Join-Path $fixture 'source'
    foreach($name in @('SurfXR Dashboard.exe','bin/win64/driver_surfxr_server.dll','surfxr-daemon.exe',
        'surfxr-desktop.exe','surfxr-desktop-module.dll','bin/win64/surfxr_frame_worker.exe','licenses/dependencies.html')) {
        Write-Fixture (Join-Path $source $name) 'fixture only'
    }
    $adapters=@('Install-Frame.ps1','launch/Start-Frame.ps1',
        'launch/frame_release.ps1','launch/frame_release_commands.ps1')
    foreach($name in $adapters) {
        $target=Join-Path $source "frame-installer/$name"
        Write-Fixture $target ''
        Copy-Item -LiteralPath (Join-Path $repository "scripts/$name") -Destination $target
    }
    Write-Fixture (Join-Path $source 'session.json') 'private fixture preferences'
    Write-Fixture (Join-Path $source 'frame-installer/not-owned.ps1') 'must not ship'
    $crt=Join-Path $fixture 'crt'
    foreach($name in @('msvcp140.dll','vcruntime140.dll','vcruntime140_1.dll')) {Write-Fixture (Join-Path $crt $name) 'fixture CRT'}
    $frame=Join-Path $fixture 'Frame'
    Write-Fixture (Join-Path $frame 'Install-Frame.ps1') 'old independent archive adapter'
    $hash=(Get-FileHash -LiteralPath (Join-Path $frame 'Install-Frame.ps1')).Hash.ToLowerInvariant()
    Write-Fixture (Join-Path $frame 'release-manifest.json') (@{format_version=2;install_profile='surfxr';source_commit='old-frame';files=@{'Install-Frame.ps1'=$hash}} | ConvertTo-Json -Depth 5)
    $archive=Join-Path $fixture 'frame.zip'
    [IO.Compression.ZipFile]::CreateFromDirectory($frame,$archive,[IO.Compression.CompressionLevel]::Optimal,$true)
    $env:SURFXR_MSVC_REDIST_DIR=$crt; $env:SURFXR_FRAME_CLIENT_ZIP=$archive; $env:SURFXR_RELEASE_ID='fixture'
    function git {
        if($args -notcontains 'rev-parse' -or $args -notcontains 'HEAD'){throw 'Unexpected Git operation'}
        $global:LASTEXITCODE=0; 'fixture-pc'
    }
    # Model an incompatible inherited Archive module: production must not call it.
    function Expand-Archive {throw 'Incompatible Archive module: ExtractToDirectory overload unavailable'}
    function Compress-Archive {throw 'Incompatible Archive module'}
    & $script -Source $source -Setup:$CompileSetup
    # Replacing an existing ZIP must remain supported too.
    & $script -Source $source
    if($CompileSetup) {
        Assert-Check (Test-Path (Join-Path $workspace 'release/SurfXR-Windows-setup.exe.sha256')) 'Setup compiles with checksums'
    }
    $expanded=Join-Path $fixture 'expanded'
    [IO.Compression.ZipFile]::ExtractToDirectory((Join-Path $workspace 'release/SurfXR-Windows-portable.zip'),$expanded)
    $payload=Join-Path $expanded 'SurfXR'
    $manifest=Get-Content -LiteralPath (Join-Path $payload 'pc-release-manifest.json') -Raw | ConvertFrom-Json
    foreach($name in $adapters) {
        $key="frame-installer/$name"
        Assert-Check ($manifest.files.PSObject.Properties.Name -contains $key) "adapter hash exists: $key"
        Assert-Check ((Get-FileHash -LiteralPath (Join-Path $payload $key)).Hash.ToLowerInvariant() -eq $manifest.files.$key) "adapter bytes verified: $key"
    }
    Assert-Check (!(Test-Path -LiteralPath (Join-Path $payload 'session.json'))) 'user settings excluded'
    Assert-Check (!(Test-Path -LiteralPath (Join-Path $payload 'frame-installer/not-owned.ps1'))) 'unowned scripts excluded'
    Assert-Check ([IO.File]::ReadAllText((Join-Path $payload 'frame-client/Install-Frame.ps1')) -eq 'old independent archive adapter') 'older Frame archive remains intact'
    $badZip=Join-Path $fixture 'traversal.zip'
    $zip=[IO.Compression.ZipFile]::Open($badZip,[IO.Compression.ZipArchiveMode]::Create)
    try { $null=$zip.CreateEntry('../escaped.txt') } finally {$zip.Dispose()}
    $env:SURFXR_FRAME_CLIENT_ZIP=$badZip
    $rejected=$false
    try {& $script -Source $source} catch {$rejected=$_.Exception.ToString() -match 'outside|outside.*directory|parent directory'}
    Assert-Check $rejected 'archive traversal is rejected'
    Assert-Check (@(Get-ChildItem $workspace -Filter escaped.txt -Recurse).Count -eq 0) 'archive traversal writes nothing outside extraction'
    $env:SURFXR_FRAME_CLIENT_ZIP=$archive
    $missing=Join-Path $source 'frame-installer/launch/frame_release_commands.ps1'
    Move-Item -LiteralPath $missing -Destination ($missing+'.missing')
    $rejected=$false
    try {& $script -Source $source} catch {$rejected=$_.Exception.Message -like '*Required release payload missing*frame_release_commands.ps1*'}
    Assert-Check $rejected 'missing adapter is rejected before packaging'
    Write-Output 'Portable checks passed: module-independent ZIP/overwrite, traversal rejection, adapter hashes, old Frame preserved, private files excluded, missing helper rejected.'
} finally {
    foreach($name in $saved.Keys){[Environment]::SetEnvironmentVariable($name,$saved[$name],'Process')}
    $resolved=[IO.Path]::GetFullPath($fixture)
    if(!$resolved.StartsWith([IO.Path]::GetFullPath($parent)+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)){throw 'Unsafe fixture cleanup'}
    if(Test-Path -LiteralPath $resolved){Remove-Item -LiteralPath $resolved -Recurse -Force}
}
