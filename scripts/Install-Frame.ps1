[CmdletBinding()]
param(
    [ValidateSet('menu','install','uninstall','purge')][string]$Action = 'menu',
    [string]$FrameSsh = 'steamos@frame',
    [ValidateSet('lan','direct')][string]$Link = 'direct',
    [string]$Python = 'python',
    [string]$ReleaseDirectory = '',
    [switch]$PairOnInstall,
    [string]$DashboardResult = '',
    [switch]$Plan
)

try {
    $ErrorActionPreference = 'Stop'
    [Console]::OutputEncoding = New-Object System.Text.UTF8Encoding($false)
    if ([string]::IsNullOrWhiteSpace($ReleaseDirectory)) { $ReleaseDirectory = $PSScriptRoot }
    $options = @{ReleaseDirectory=$ReleaseDirectory; ReleaseAction=$Action}
    foreach ($name in @('FrameSsh','Link','Python','Plan','PairOnInstall','DashboardResult')) {
        if ($PSBoundParameters.ContainsKey($name)) { $options[$name] = $PSBoundParameters[$name] }
    }
    & (Join-Path $PSScriptRoot 'launch/Start-Frame.ps1') @options
} catch {
    # Also retain failures before the transport/result writer has been loaded.
    if ($DashboardResult -and $_.Exception.Data['SurfXRDashboardResultWritten'] -ne $DashboardResult -and
        (Test-Path -LiteralPath $DashboardResult -PathType Leaf)) {
        [IO.File]::AppendAllText($DashboardResult, "`nInstallation failed: " + $_.Exception.Message + "`n", (New-Object Text.UTF8Encoding($false)))
    }
    throw
}
