# Private command descriptions shared by the release executor and its dry run.
function Get-ReleaseSshArguments([string]$Target, [string]$Command, [switch]$Ipv4) {
    if ($Ipv4) { '-4' }
    @('-T', '-o', 'BatchMode=no', '-o', 'StrictHostKeyChecking=ask', '-o', 'ConnectTimeout=10',
      '-o', 'ServerAliveInterval=5', '-o', 'ServerAliveCountMax=2', $Target, $Command)
}

function New-FrameReleaseCommands([string]$Directory, [string]$Target, [string]$Link, $Tools,
                                  [string]$Id, [string]$RemoteRoot) {
    $native = "$RemoteRoot/build/surfxr-client"
    [pscustomobject]@{
        Prepare = 'umask 077; test "$(uname -m)" = aarch64 || exit 1; root="$HOME/.cache/surfxr-client/releases/' + $Id + '"; mkdir -p "$root" || exit 1; printf ''{"ok":true,"root":"%s"}\n'' "$root"'
        ScpArguments = @('-S', $Tools.Ssh, '-o', 'BatchMode=no', '-o', 'StrictHostKeyChecking=ask', '-r',
            (Join-Path $Directory 'payload/build'), (Join-Path $Directory 'release-manifest.json'), "${Target}:$RemoteRoot/")
        Permission = "chmod 755 '$native' && " + 'printf ''{"ok":true}\n'''
        Verify = "$native --deploy verify-release"
        Inspect = "$native --deploy inspect"
        Install = "$native --deploy install"
        Request = ([ordered]@{mode='desktop'; link=$Link; pc_lan=''; pc_direct=''; frame_lan=''; frame_direct=''} | ConvertTo-Json -Compress)
        Identity = 'name=$(hostname | head -c 63); name=$(printf "%s" "$name" | LC_ALL=C tr -c "A-Za-z0-9._-" "-"); set -- $SSH_CONNECTION; printf ''{"ok":true,"hostname":"%s","ip":"%s"}\n'' "$name" "$3"'
    }
}

function Format-ReleaseCommand([string]$Executable, [string[]]$Arguments) {
    $quoted = @($Executable) + $Arguments | ForEach-Object { "'" + $_.Replace("'", "''") + "'" }
    '& ' + ($quoted -join ' ')
}

function Write-ReleaseSshPlan([string]$Target, [string]$Ssh, [string]$Command, [string]$InputText, [switch]$Ipv4) {
    Write-Output (Format-ReleaseCommand $Ssh (Get-ReleaseSshArguments $Target $Command -Ipv4:$Ipv4))
    Write-Output '# Remote shell command (exact command argument):'
    Write-Output $Command
    Write-Output '# UTF-8 stdin:'
    Write-Output $InputText
    Write-Output ''
}

function Show-FrameReleasePlan([string]$Directory, [string]$Action, [string]$Target,
                              [string]$Python, [string]$Link, [bool]$PairOnInstall) {
    if ($Target -notmatch '^[A-Za-z0-9_.-]+@[A-Za-z0-9_.-]+$' -or $Target.StartsWith('-')) { throw 'SSH target must be user@hostname or user@IPv4.' }
    $manifest = Assert-FrameRelease $Directory
    $tools = Get-ReleaseTools $Directory $Python $manifest
    Write-Output "# Release $Action via ${Target}. Preview only: no SSH/SCP or remote changes."
    Write-Output "# Local manifest SHA-256 verification passed: $Directory"
    if ($Action -in @('uninstall','purge')) {
        if ($Action -eq 'purge') { Write-Output '# Full uninstall deletes the Frame IP memory, settings, layouts, logs and release cache.' }
        $key = 'surfxr/frame/launcher/native/residual_remove.sh.in'
        if (-not ($manifest.files.PSObject.Properties.Name -contains $key)) { throw 'Release removal adapter is missing.' }
        $body = [IO.File]::ReadAllText((Join-Path $Directory $key)).Replace("`r`n", "`n")
        Write-ReleaseSshPlan $Target $tools.Ssh "bash -s -- $Action" $body
        return
    }
    if ($Action -ne 'install') { throw 'Choose install, uninstall or purge before previewing commands.' }
    $id = [Guid]::NewGuid().ToString('N')
    $commands = New-FrameReleaseCommands $Directory $Target $Link $tools $id '${REMOTE_ROOT}'
    Write-Output '# Each execution uses a fresh staging ID. ${REMOTE_ROOT} is replaced with the validated absolute root returned by step 1.'
    Write-ReleaseSshPlan $Target $tools.Ssh $commands.Prepare ''
    Write-Output (Format-ReleaseCommand $tools.Scp $commands.ScpArguments)
    Write-Output ''
    Write-ReleaseSshPlan $Target $tools.Ssh $commands.Permission ''
    Write-ReleaseSshPlan $Target $tools.Ssh $commands.Verify '{}'
    Write-ReleaseSshPlan $Target $tools.Ssh $commands.Inspect '{}'
    Write-ReleaseSshPlan $Target $tools.Ssh $commands.Install $commands.Request
    Write-Output '# --deploy install runs the package native launcher installation logic, not a hidden shell script; streaming remains idle.'
    if ($PairOnInstall) { Write-ReleaseSshPlan $Target $tools.Ssh $commands.Identity '' -Ipv4 }
}
