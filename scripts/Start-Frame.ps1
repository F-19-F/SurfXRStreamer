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

# The PC launch module owns the single-link pipeline.
& (Join-Path $PSScriptRoot 'launch/Start-Frame.ps1') @PSBoundParameters
