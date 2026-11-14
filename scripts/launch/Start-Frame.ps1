<#
.SYNOPSIS
Build, deploy and start the single-link Frame client.
.DESCRIPTION
Reuses build_viewer.py, frame_remote.py and the installed launcher IPC.
BuildOnly never installs, pairs, starts or stops services.
#>
[CmdletBinding()]
param(
    [ValidateSet('desktop','vr')][string]$Mode = 'desktop',
    [ValidateSet('lan','direct')][string]$Link = 'direct',
    [string]$FrameSsh = 'steamos@frame',
    [string]$PcLan = '',
    [string]$PcDirect = '',
    [string]$FrameLan = '',
    [string]$FrameDirect = '',
    [string]$Python = 'python',
    [switch]$BuildOnly,
    [switch]$NoStart,
    [switch]$PairOnInstall,
    [string]$DashboardResult = '',
    [switch]$Plan,
    [string]$ReleaseDirectory = '',
    [ValidateSet('menu','install','uninstall','purge')][string]$ReleaseAction = 'install'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$workspaceRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$project = Join-Path $workspaceRoot 'surfxr'
$buildReport = Join-Path $workspaceRoot 'build/reports/frame-ssh/validation.json'
$remoteTool = Join-Path $PSScriptRoot 'frame_remote.py'
$stage = 'preflight'

if ($ReleaseDirectory) {
    if (-not $PSBoundParameters.ContainsKey('FrameSsh')) { $FrameSsh = 'steamos@frame' }
    if ($BuildOnly) { throw 'Release installation cannot be combined with BuildOnly.' }
    . (Join-Path $PSScriptRoot 'frame_release.ps1')
    if ($DashboardResult) {
        if ($Plan -or $ReleaseAction -notin @('install','uninstall') -or
            ($ReleaseAction -eq 'install' -and -not $PairOnInstall) -or
            ($ReleaseAction -eq 'uninstall' -and $PairOnInstall)) { throw 'DashboardResult requires installation with pairing or uninstall without pairing.' }
        Invoke-DashboardFrameRelease -ResultFile $DashboardResult -Directory $ReleaseDirectory -Target $FrameSsh -Python $Python -Link $Link -Action $ReleaseAction -PairOnInstall:$PairOnInstall
    } else {
        Invoke-FrameRelease -Directory $ReleaseDirectory -Action $ReleaseAction -Target $FrameSsh -Python $Python -Link $Link -Plan:$Plan -PairOnInstall:$PairOnInstall
    }
    return
}

if ($PairOnInstall -or $DashboardResult) { throw 'PairOnInstall requires a release installation from Dashboard.' }

function Invoke-Python([string[]]$Arguments) {
    & $Python @Arguments
    if ($LASTEXITCODE -ne 0) { throw "Python stage failed (exit $LASTEXITCODE)." }
}

function Invoke-Frame([string]$Command) {
    $options = @($remoteTool, $Command, '--ssh', $FrameSsh, '--build-report', $buildReport,
        '--mode', $Mode.ToLowerInvariant(), '--link', $Link.ToLowerInvariant())
    if ($PcLan) { $options += @('--pc-lan', $PcLan) }
    if ($PcDirect) { $options += @('--pc-direct', $PcDirect) }
    if ($FrameLan) { $options += @('--frame-lan', $FrameLan) }
    if ($FrameDirect) { $options += @('--frame-direct', $FrameDirect) }
    Invoke-Python $options
}

if ($Plan) {
    Write-Output "Build/check only the Frame ARM64 client over SSH ($FrameSsh); SURFXR only."
    if (-not $BuildOnly) {
        Write-Output "Disconnect this client's managed viewer; install Frame binaries/services and configure single $Link link."
        if (-not $NoStart) { Write-Output "Use the PC SurfXR Dashboard; start Frame SteamVR/dashboard/viewer ($Mode)." }
    }
    return
}
if ($env:OS -ne 'Windows_NT') { throw 'Run Start-FrameSingle.ps1 on the Windows PC.' }
if ($FrameSsh -notmatch '^[A-Za-z0-9_.-]+@[A-Za-z0-9_.:-]+$' -or $FrameSsh.StartsWith('-')) {
    throw 'FrameSsh must be user@host without shell syntax.'
}
foreach ($command in @($Python, 'ssh.exe', 'scp.exe')) { Get-Command $command -ErrorAction Stop | Out-Null }
Invoke-Python @('-c', 'import sys; assert sys.version_info >= (3,10), "Python 3.10+ is required"')
Invoke-Python @((Join-Path $workspaceRoot 'scripts/build/check_structure.py'))
if (-not $BuildOnly) {
    $addresses = @(Get-NetIPAddress -AddressFamily IPv4 -ErrorAction Stop | Where-Object AddressState -eq 'Preferred' | Select-Object -ExpandProperty IPAddress)
    $required = if ($Link -eq 'direct') { $PcDirect } else { $PcLan }
    if ($required -and $required -notin $addresses) { throw "PC address $required is unavailable; pass the current link address." }
}
try {
    $stage = 'Frame build/offline checks'
    Write-Host '[1/4] Building and checking the single-link Frame client...'
    Invoke-Python @((Join-Path $workspaceRoot 'scripts/build/build_viewer.py'), '--standalone', '--ssh', $FrameSsh, '--output', $buildReport)
    if ($BuildOnly) { Write-Host 'Frame build and offline checks passed. Nothing was deployed or started.'; return }

    $stage = 'Frame preflight/stop'
    Write-Host '[2/4] Preparing the managed Frame client for deployment...'
    Invoke-Frame 'inspect'
    Invoke-Frame 'stop'
    $stage = 'Frame deployment'
    Write-Host '[3/4] Installing the Frame viewer/dashboard/connection service...'
    Invoke-Frame 'deploy'
    if ($NoStart) { Write-Host 'Deployment complete. The connection service is idle; streaming was not started.'; return }
    $stage = 'Frame startup'
    Write-Host '[4/4] Starting Frame streaming...'
    Invoke-Frame 'start'
    Write-Host "Frame reports streaming in $Mode mode (single $Link link). Wearer acceptance remains separate."
} catch {
    Write-Host "Failed at stage: $stage" -ForegroundColor Red
    Write-Warning 'The failure handler did not disconnect the viewer. Check Dashboard for its current state and any client failure reason.'
    throw
}
