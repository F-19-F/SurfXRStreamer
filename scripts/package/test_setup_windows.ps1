# Exercise the real Setup/Uninstall in an isolated directory. No app/service launch.
param([Parameter(Mandatory=$true)][string]$Installer)
$ErrorActionPreference = 'Stop'
$installerPath = (Resolve-Path -LiteralPath $Installer).Path
$key = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\SurfXR'
$appKey = 'HKCU:\Software\SurfXR'
$links = Join-Path ([Environment]::GetFolderPath('Programs')) 'SurfXR'
if ((Test-Path $key) -or (Test-Path $appKey) -or (Test-Path -LiteralPath $links)) {
    throw 'Use a clean test account: an SurfXR Setup registration or shortcut already exists'
}
$repositoryRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$testRoot = Join-Path $repositoryRoot ('build/tests/setup-' + [guid]::NewGuid().ToString('N'))
$target = Join-Path $testRoot 'with spaces'
New-Item -ItemType Directory -Path $target -Force | Out-Null
$config = Join-Path $target 'session.json'
[IO.File]::WriteAllText($config, '{"setup_test":"preserve"}')
[IO.File]::WriteAllText((Join-Path $target 'user-note.txt'), 'preserve user files')
function Assert-Check([bool]$Value, [string]$Message) {
    if (!$Value) { throw "Setup validation failed: $Message" }
}
function Assert-Payload {
    $manifest = Get-Content -LiteralPath (Join-Path $target 'pc-release-manifest.json') -Raw | ConvertFrom-Json
    foreach ($entry in $manifest.files.PSObject.Properties) {
        $path = Join-Path $target $entry.Name
        Assert-Check ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() -eq $entry.Value) $entry.Name
    }
    Assert-Check ((Get-Content -LiteralPath $config -Raw) -eq '{"setup_test":"preserve"}') 'settings preserved'
    Assert-Check ((Get-ItemProperty $key).InstallLocation -eq $target) 'uninstall registration'
    Assert-Check ((Get-ItemProperty $key).UninstallString -eq ('"' + $target + '\Uninstall.exe"')) 'quoted uninstall path'
    Assert-Check (Test-Path -LiteralPath (Join-Path $links 'Install Steam Frame.lnk')) 'headset installer shortcut'
    return $manifest
}
foreach ($pass in 1..2) {
    # NSIS /D must be last and consumes the entire remaining unquoted path.
    $process = Start-Process -FilePath $installerPath -ArgumentList "/S /D=$target" -PassThru -WindowStyle Hidden
    if (!$process.WaitForExit(120000)) { throw 'Setup timed out' }
    Assert-Check ($process.ExitCode -eq 0) "install/upgrade exit ($pass)"
    $manifest = Assert-Payload
}
$process = Start-Process -FilePath (Join-Path $target 'Uninstall.exe') -ArgumentList '/S' -PassThru -WindowStyle Hidden
if (!$process.WaitForExit(30000)) { throw 'Uninstall timed out' }
$deadline = [DateTime]::UtcNow.AddSeconds(30)
while ((Test-Path $key) -and [DateTime]::UtcNow -lt $deadline) { Start-Sleep -Milliseconds 100 }
Assert-Check (!(Test-Path $key) -and !(Test-Path $appKey)) 'registration removed'
Assert-Check (!(Test-Path -LiteralPath $links)) 'shortcuts removed'
foreach ($name in $manifest.files.PSObject.Properties.Name) {
    Assert-Check (!(Test-Path -LiteralPath (Join-Path $target $name))) "payload removed: $name"
}
Assert-Check ((Get-Content -LiteralPath $config -Raw) -eq '{"setup_test":"preserve"}') 'settings preserved after uninstall'
Assert-Check (Test-Path -LiteralPath (Join-Path $target 'user-note.txt')) 'user files preserved after uninstall'
Write-Output "Setup checks passed: fresh install, upgrade, $($manifest.files.PSObject.Properties.Name.Count) hashes, shortcuts, uninstall, preserved settings. Fixture: $testRoot"
