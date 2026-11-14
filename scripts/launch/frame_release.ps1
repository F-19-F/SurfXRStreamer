# Private release transport for Start-Frame.ps1. OpenSSH owns password prompts.
. (Join-Path $PSScriptRoot 'frame_release_commands.ps1')
function Invoke-DashboardFrameRelease {
    param([string]$ResultFile, [string]$Directory, [string]$Target, [string]$Python, [string]$Link, [string]$Action, [switch]$PairOnInstall)
    # Dashboard pre-creates a unique file. Keep native stdin/stderr and the console
    # intact so OpenSSH can show host keys and own password entry independently.
    $stream = [System.IO.File]::Open($ResultFile, [System.IO.FileMode]::Truncate, [System.IO.FileAccess]::Write, [System.IO.FileShare]::Read)
    $writer = New-Object System.IO.StreamWriter($stream, (New-Object System.Text.UTF8Encoding($false)))
    try {
        Invoke-FrameRelease -Directory $Directory -Action $Action -Target $Target -Python $Python -Link $Link -PairOnInstall:$PairOnInstall 3>&1 6>&1 | ForEach-Object {
            $writer.WriteLine($_.ToString())
            Write-Host $_
        }
    } catch {
        $writer.WriteLine('Installation failed: ' + $_.Exception.Message)
        $_.Exception.Data['SurfXRDashboardResultWritten'] = $ResultFile
        throw
    } finally { $writer.Dispose() }
}

function Resolve-FrameReleaseTarget([string]$Target, [bool]$Dashboard = $false) {
    if ($Target.StartsWith('-') -or $Target -notmatch '^([A-Za-z0-9_.-]+)@([A-Za-z0-9_.-]+)$') { throw 'SSH target must be user@hostname or user@IPv4.' }
    $login = $Matches[1]
    $hostname = $Matches[2]
    try {
        $addresses = @([System.Net.Dns]::GetHostAddresses($hostname))
        if ($addresses.Count) { return $Target }
    } catch { }
    if ($Dashboard) { throw "Cannot resolve $hostname. Enter user@IPv4 in the Dashboard Frame SSH field and retry." }
    Write-Host "Cannot resolve $hostname. Enter the Steam Frame IPv4 address."
    while ($true) {
        $address = Read-Host 'Steam Frame IPv4'
        $parsed = $null
        if ([System.Net.IPAddress]::TryParse($address.Trim(), [ref]$parsed) -and
            $parsed.AddressFamily -eq [System.Net.Sockets.AddressFamily]::InterNetwork -and
            -not [System.Net.IPAddress]::IsLoopback($parsed) -and $parsed.ToString() -ne '0.0.0.0') {
            return "$login@$($parsed.ToString())"
        }
        Write-Host 'Enter an IPv4 address such as 192.168.1.50.'
    }
}

function Assert-FrameRelease([string]$Directory) {
    $root = [System.IO.Path]::GetFullPath($Directory)
    $manifest = Get-Content -LiteralPath (Join-Path $root 'release-manifest.json') -Raw -Encoding UTF8 | ConvertFrom-Json
    if ($manifest.format_version -ne 2 -or $manifest.architecture -ne 'aarch64' -or $manifest.frame_python_required -ne $false) {
        throw 'This package is not a supported single-link ARM64 release.'
    }
    if ($manifest.PSObject.Properties.Name -contains 'install_profile' -and $manifest.install_profile -notin @('surfxr')) {
        throw 'Unsupported release installation profile.'
    }
    foreach ($entry in $manifest.files.PSObject.Properties) {
        if ($entry.Name -notmatch '^[A-Za-z0-9_./-]+$' -or $entry.Name.Split('/') -contains '..' -or $entry.Name.StartsWith('/')) {
            throw 'Invalid release file path.'
        }
        $file = Join-Path $root $entry.Name
        if ((Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant() -ne $entry.Value) {
            throw "Release file hash mismatch: $($entry.Name)"
        }
    }
    foreach ($binary in @('surfxr-viewer','surfxr-dashboard','surfxr-discover','surfxr-client','surfxr-runtime-bridge')) {
        if (-not ($manifest.files.PSObject.Properties.Name -contains "payload/build/$binary")) { throw "Missing release binary: $binary" }
    }
    return $manifest
}

function Get-ReleaseTools([string]$Directory, [string]$Python, $Manifest) {
    $root = [System.IO.Path]::GetFullPath($Directory)
    $bundled = $Manifest.PSObject.Properties.Name -contains 'pc_tools'
    if ($bundled) {
        if (-not [Environment]::Is64BitOperatingSystem) { throw 'This portable release requires 64-bit Windows 10 or Windows 11.' }
        $tools = @{Ssh=(Join-Path $root 'tools/windows-x64/openssh/ssh.exe'); Scp=(Join-Path $root 'tools/windows-x64/openssh/scp.exe')}
        foreach ($name in @('tools/windows-x64/openssh/ssh.exe','tools/windows-x64/openssh/scp.exe','tools/windows-x64/python/python.exe')) {
            if (-not ($Manifest.files.PSObject.Properties.Name -contains $name) -or -not (Test-Path -LiteralPath (Join-Path $root $name) -PathType Leaf)) {
                throw "Bundled tool is missing from this release: $name. Extract the complete ZIP again."
            }
        }
        $tools.Python = if ($Python -in @('','python','python.exe')) { Join-Path $root 'tools/windows-x64/python/python.exe' } else { $Python }
        return $tools
    }
    # Keep older native-format packages usable with their original system tools.
    $ssh = Get-Command ssh.exe -ErrorAction SilentlyContinue
    $scp = Get-Command scp.exe -ErrorAction SilentlyContinue
    if (-not $ssh -or -not $scp) { throw 'This older package has no SSH tools. Use the portable release ZIP or install Windows OpenSSH Client.' }
    return @{Ssh=$ssh.Source; Scp=$scp.Source; Python=$Python}
}

function ConvertTo-ReleaseNativeArgument([string]$Value) {
    # Windows argv quoting: double backslashes before a quote or the final quote.
    # PowerShell 5.1's native binder otherwise removes quotes inside SSH commands.
    $escaped = [regex]::Replace($Value, '(\\*)"', '$1$1\"')
    return '"' + [regex]::Replace($escaped, '(\\+)$', '$1$1') + '"'
}

function Invoke-ReleaseSsh([string]$Target, [string]$Command, [string]$InputText, [string]$Ssh='ssh.exe', [switch]$Ipv4) {
    $start = New-Object System.Diagnostics.ProcessStartInfo
    $start.FileName = $Ssh
    $arguments = Get-ReleaseSshArguments $Target $Command -Ipv4:$Ipv4
    $start.Arguments = ($arguments | ForEach-Object { ConvertTo-ReleaseNativeArgument $_ }) -join ' '
    $start.UseShellExecute = $false
    $start.RedirectStandardInput = $true
    $start.RedirectStandardOutput = $true
    $start.StandardOutputEncoding = New-Object System.Text.UTF8Encoding($false)
    # Inherit stderr and the console so OpenSSH still owns host-key/password prompts.
    $process = New-Object System.Diagnostics.Process
    $process.StartInfo = $start
    try {
        $null = $process.Start()
        $read = $process.StandardOutput.ReadToEndAsync()
        # Account data is UTF-8 on stdin only, never in the Windows command line.
        $bytes = [System.Text.Encoding]::UTF8.GetBytes($InputText + "`n")
        $process.StandardInput.BaseStream.Write($bytes, 0, $bytes.Length)
        $process.StandardInput.Close()
        $process.WaitForExit()
        $output = $read.GetAwaiter().GetResult()
        $sshFailure = "SSH operation failed (exit code $($process.ExitCode)). Check the address, SSH access and password."
        try { $result = ConvertFrom-Json -InputObject $output -ErrorAction Stop }
        catch {
            if ($process.ExitCode -ne 0) { throw $sshFailure }
            throw 'Frame returned invalid JSON. Use the complete updated release ZIP; if this persists, check SSH login shell startup messages. Operation success was not confirmed.'
        }
        if ($null -eq $result -or $result -is [array] -or -not ($result.PSObject.Properties.Name -contains 'ok') -or $result.ok -isnot [bool]) {
            if ($process.ExitCode -ne 0) { throw $sshFailure }
            throw 'Frame returned an unexpected response without operation status. Operation success was not confirmed.'
        }
        if ($process.ExitCode -eq 255) { throw $sshFailure }
        if (-not $result.ok) {
            $code = 'frame_operation_failed'
            if ($result.PSObject.Properties.Name -contains 'error' -and $result.error -is [string] -and $result.error -cmatch '^[a-z_]{1,128}$') { $code = $result.error }
            $advice = switch ($code) {
                'instance_active' { 'A Frame client still holds an instance lock. Close it and retry removal.' }
                'purge_path_unverified' { 'A cleanup directory is redirected or has unexpected ownership. No unverified data root was deleted.' }
                'frame_uninstaller_missing' { 'The application directory remains but its native uninstaller is missing. Repair the installation before removing it.' }
                'frame_user_service_failed' { 'The Frame user service manager could not complete the operation. Check the SteamOS user session.' }
                default { 'Operation success was not confirmed.' }
            }
            throw "Frame operation failed: $code (remote exit code $($process.ExitCode)). $advice"
        }
        if ($process.ExitCode -ne 0) { throw "Frame reported success but exited with code $($process.ExitCode). Operation success was not confirmed." }
        return $result
    } finally { $bytes = $null; $output = $null; $process.Dispose() }
}

function ConvertTo-FramePairingReceipt($Identity) {
    if (-not ($Identity.PSObject.Properties.Name -contains 'hostname') -or
        $Identity.hostname -isnot [string] -or $Identity.hostname -cnotmatch '^[A-Za-z0-9_.-]{1,63}$') {
        throw 'Frame returned an invalid pairing hostname.'
    }
    $parsed = $null
    if (-not ($Identity.PSObject.Properties.Name -contains 'ip') -or $Identity.ip -isnot [string] -or
        -not [System.Net.IPAddress]::TryParse($Identity.ip, [ref]$parsed) -or
        $parsed.AddressFamily -ne [System.Net.Sockets.AddressFamily]::InterNetwork -or
        [System.Net.IPAddress]::IsLoopback($parsed) -or $parsed.Equals([System.Net.IPAddress]::Any)) {
        throw 'Frame did not return a usable IPv4 pairing address. Check IPv4 connectivity to the SSH target.'
    }
    'SURFXR_PAIR_ID ' + (@{hostname=$Identity.hostname; ip=$parsed.ToString()} | ConvertTo-Json -Compress)
}

function Invoke-FrameRelease {
    param([string]$Directory, [string]$Action, [string]$Target, [string]$Python, [string]$Link, [switch]$Plan, [switch]$PairOnInstall)
    if ($PairOnInstall -and $Action -ne 'install') { throw 'PairOnInstall requires Action install.' }
    if ($Plan) {
        Show-FrameReleasePlan $Directory $Action $Target $Python $Link $PairOnInstall
        return
    }
    if ($env:OS -ne 'Windows_NT') { throw 'Run this release installer on Windows.' }
    if ($Action -eq 'menu') {
        Write-Host '1. Install / update Frame client'
        Write-Host '2. Uninstall Frame client (keep settings)'
        Write-Host '3. Fully uninstall Frame client (DELETE settings/layouts/logs/cache)'
        Write-Host '0. Exit'
        $choice = Read-Host 'Select [1]'
        if ($choice -eq '0') { return }
        if ($choice -notin @('','1','2','3')) { throw 'Select 1, 2, 3, or 0.' }
        $Action = if ($choice -eq '3') { 'purge' } elseif ($choice -eq '2') { 'uninstall' } else { 'install' }
    }
    $manifest = Assert-FrameRelease $Directory
    $surfxrProfile = $manifest.PSObject.Properties.Name -contains 'install_profile' -and $manifest.install_profile -eq 'surfxr'
    $tools = Get-ReleaseTools $Directory $Python $manifest
    $Target = Resolve-FrameReleaseTarget $Target $PairOnInstall
    Write-Host "Connecting to $Target. Enter the password if prompted."
    if ($Action -in @('uninstall','purge')) {
        $script = Join-Path $Directory 'surfxr/frame/launcher/native/residual_remove.sh.in'
        $key = 'surfxr/frame/launcher/native/residual_remove.sh.in'
        if (-not ($manifest.files.PSObject.Properties.Name -contains $key)) { throw 'This release is missing its SSH removal adapter. Extract the complete updated ZIP.' }
        $body = [System.IO.File]::ReadAllText($script).Replace("`r`n", "`n")
        $result = Invoke-ReleaseSsh $Target "bash -s -- $Action" $body $tools.Ssh
        if (-not ($result.PSObject.Properties.Name -contains 'uninstalled') -or $result.uninstalled -isnot [bool] -or $result.uninstalled -ne $true -or
            -not ($result.PSObject.Properties.Name -contains 'configuration_preserved') -or
            $result.configuration_preserved -isnot [bool] -or
            $result.configuration_preserved -ne ($Action -eq 'uninstall')) { throw 'Frame did not confirm the requested removal.' }
        if ($Action -eq 'purge') { Write-Host 'Frame client fully uninstalled. IP memory, settings, layouts, logs and release cache were deleted.' }
        else { Write-Host 'Frame client uninstalled. Saved connections and settings were preserved.' }
        return
    }
    $id = [Guid]::NewGuid().ToString('N')
    $commands = New-FrameReleaseCommands $Directory $Target $Link $tools $id ''
    $prepared = Invoke-ReleaseSsh $Target $commands.Prepare '' $tools.Ssh
    $remoteRoot = [string]$prepared.root
    if ($remoteRoot -notmatch '^/home/[A-Za-z0-9_.-]+/\.cache/surfxr-client/releases/[a-f0-9]{32}$') { throw 'Unexpected Frame staging directory.' }
    $commands = New-FrameReleaseCommands $Directory $Target $Link $tools $id $remoteRoot
    # scp starts ssh as a subprocess. Explicit -S avoids a system PATH dependency.
    $scpArguments = $commands.ScpArguments
    & $tools.Scp @scpArguments
    if ($LASTEXITCODE -ne 0) { throw 'Release transfer failed. Retry the installer.' }
    $null = Invoke-ReleaseSsh $Target $commands.Permission '' $tools.Ssh
    $null = Invoke-ReleaseSsh $Target $commands.Verify '{}' $tools.Ssh
    $request = $commands.Request
    try {
        $inspection = Invoke-ReleaseSsh $Target $commands.Inspect '{}' $tools.Ssh
        $result = Invoke-ReleaseSsh $Target $commands.Install $request $tools.Ssh
        if ($surfxrProfile -and (-not ($result.PSObject.Properties.Name -contains 'backend') -or $result.backend -ne 'surfxr')) {
            throw 'SurfXR installation did not confirm the SURFXR backend. Do not start streaming; install the complete updated package.'
        }
        Write-Host 'Frame installation complete.'
        if ($result.PSObject.Properties.Name -contains 'steamvr_restart_required' -and $result.steamvr_restart_required -eq $true) {
            Write-Host 'Controller input driver registered. Restart SteamVR on the headset to enable full controller capture and finger tracking.'
        }
        if ($result.PSObject.Properties.Name -contains 'dashboard_started' -and $result.dashboard_started -is [bool] -and $result.dashboard_started -eq $true) {
            if (-not $PairOnInstall) { Write-Host 'SurfXR is installed. Open PC SurfXR Dashboard and pair this Frame in Devices.' }
        } else {
            $reason = 'frame_dashboard_start_failed'
            if ($result.PSObject.Properties.Name -contains 'dashboard_error' -and $result.dashboard_error -is [string] -and
                $result.dashboard_error -cmatch '^[a-z_]{1,128}$') { $reason = $result.dashboard_error }
            Write-Warning "Could not open SurfXR ($reason). Open SurfXR from the headset desktop."
        }
        if ($PairOnInstall) {
            # Read identity over the same authenticated SSH target, not network discovery.
            # Admission uses IPv4. A remote IPv6 link-local scope (e.g. %wlanap)
            # identifies the Frame interface, not a usable Windows pairing address.
            try {
                $identity = Invoke-ReleaseSsh $Target $commands.Identity '' $tools.Ssh -Ipv4
                ConvertTo-FramePairingReceipt $identity
            } catch { throw ('Frame installation completed, but automatic pairing failed: ' + $_.Exception.Message) }
        }
    } finally { $request = $null }
}
