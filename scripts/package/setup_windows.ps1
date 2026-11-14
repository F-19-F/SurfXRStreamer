# Private Setup compiler step. The portable manifest is the only payload list.
param([Parameter(Mandatory=$true)][string]$Payload,
      [Parameter(Mandatory=$true)][string]$OutputDirectory)
$ErrorActionPreference = 'Stop'
$payloadRoot = (Resolve-Path -LiteralPath $Payload).Path
$outputRoot = (Resolve-Path -LiteralPath $OutputDirectory).Path
$compiler = (Resolve-Path -LiteralPath $env:SURFXR_MAKENSIS).Path
$version = & $compiler /VERSION
if ($LASTEXITCODE -ne 0 -or $version -notmatch '^v3\.') { throw 'NSIS 3 is required' }
$manifest = Get-Content -LiteralPath (Join-Path $payloadRoot 'pc-release-manifest.json') -Raw | ConvertFrom-Json
$files = @($manifest.files.PSObject.Properties.Name) + 'pc-release-manifest.json'
$directories = @{}
$removal = @('; Generated from the verified portable manifest; no recursive deletion.')
foreach ($name in $files) {
    $path = [IO.Path]::GetFullPath((Join-Path $payloadRoot $name))
    if (!$path.StartsWith($payloadRoot + '\', [StringComparison]::OrdinalIgnoreCase) -or
        $name -match '["$\r\n]') { throw "Unsafe Setup payload path: $name" }
    if ($name -ne 'pc-release-manifest.json' -and
        (Get-PayloadHash $path) -ne $manifest.files.$name) { throw "Setup payload mismatch: $name" }
    $relative = $name.Replace('/','\')
    $removal += 'Delete "$INSTDIR\' + $relative + '"'
    $parent = Split-Path -Parent $relative
    while ($parent) {
        $directories[$parent] = $true
        $parent = Split-Path -Parent $parent
    }
}
foreach ($directory in ($directories.Keys | Sort-Object Length -Descending)) {
    $removal += 'RMDir "$INSTDIR\' + $directory + '"'
}
$include = Join-Path (Split-Path -Parent $payloadRoot) 'uninstall-payload.nsh'
[IO.File]::WriteAllLines($include, $removal, (New-Object Text.UTF8Encoding($false)))
$installer = Join-Path $outputRoot 'SurfXR-Windows-setup.exe'
& $compiler /V2 /WX "/DPAYLOAD=$payloadRoot" "/DOUTPUT=$installer" "/DREMOVE_PAYLOAD=$include" `
    "/DRELEASE_ID=$($manifest.release_id)" (Join-Path $PSScriptRoot 'setup_windows.nsi')
if ($LASTEXITCODE -ne 0) { throw "NSIS failed: $LASTEXITCODE" }
[IO.File]::WriteAllText(($installer + '.sha256'), (Get-PayloadHash $installer) + "`n")
Write-Output "Setup package: $installer"
