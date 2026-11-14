$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'frame_release.ps1')
$fixtureParent = [System.IO.Path]::GetFullPath([System.IO.Path]::GetTempPath())
$fixtureRoot = Join-Path $fixtureParent ('surfxr-frame-release-check-' + [Guid]::NewGuid().ToString('N') + ' with spaces')
$previousFixtureLog = $env:SURFXR_RELEASE_FIXTURE_LOG
$previousDashboardFailure = $env:SURFXR_RELEASE_FIXTURE_DASHBOARD_FAILURE
$previousMissingLibrary = $env:SURFXR_RELEASE_FIXTURE_MISSING_LIBRARY
$previousPairingFailure = $env:SURFXR_RELEASE_FIXTURE_PAIRING_FAILURE
$env:SURFXR_RELEASE_FIXTURE_PAIRING_FAILURE = $null
$env:SURFXR_RELEASE_FIXTURE_MISSING_LIBRARY = $null
$env:SURFXR_RELEASE_FIXTURE_LOG = $null
$env:SURFXR_RELEASE_FIXTURE_DASHBOARD_FAILURE = $null
function Assert-Check([bool]$Value, [string]$Name) { if (-not $Value) { throw "Release check failed: $Name" } }
try {
    $files = @{}
    foreach ($name in @('payload/build/surfxr-viewer','payload/build/surfxr-dashboard','payload/build/surfxr-discover','payload/build/surfxr-client','payload/build/surfxr-runtime-bridge',
                       'tools/windows-x64/openssh/ssh.exe','tools/windows-x64/openssh/scp.exe','tools/windows-x64/python/python.exe')) {
        $file = Join-Path $fixtureRoot $name
        $null = New-Item -ItemType Directory -Path (Split-Path $file) -Force
        [System.IO.File]::WriteAllText($file, 'isolated-fixture')
        $files[$name] = (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant()
    }
    $adapter = 'surfxr/frame/launcher/native/residual_remove.sh.in'
    $adapterPath = Join-Path $fixtureRoot $adapter
    $null = New-Item -ItemType Directory -Path (Split-Path $adapterPath) -Force
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot '../../surfxr/frame/launcher/native/residual_remove.sh.in') -Destination $adapterPath
    $files[$adapter] = (Get-FileHash -LiteralPath $adapterPath -Algorithm SHA256).Hash.ToLowerInvariant()
    $manifest = @{format_version=2; architecture='aarch64'; install_profile="surfxr"; frame_python_required=$false; files=$files; pc_tools=@{platform='windows-x64'}}
    $manifestFile = Join-Path $fixtureRoot 'release-manifest.json'
    $manifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $manifestFile -Encoding UTF8
    $validated = Assert-FrameRelease $fixtureRoot
    $selected = Get-ReleaseTools $fixtureRoot 'python' $validated
    Assert-Check ($selected.Ssh -eq (Join-Path $fixtureRoot 'tools/windows-x64/openssh/ssh.exe')) 'bundled SSH selection'
    Assert-Check ($selected.Scp -eq (Join-Path $fixtureRoot 'tools/windows-x64/openssh/scp.exe')) 'bundled SCP selection'
    Assert-Check ($selected.Python -eq (Join-Path $fixtureRoot 'tools/windows-x64/python/python.exe')) 'bundled Python selection'
    Assert-Check ((Get-ReleaseTools $fixtureRoot 'explicit-python-fixture' $validated).Python -eq 'explicit-python-fixture') 'explicit Python override'

    # A real Windows argv consumer catches the 5.1 binder stripping JSON quotes.
    # Compile with the OS .NET Framework compiler, also when tests run under PS 7.
    $fixtureSource = @'
using System;
using System.IO;
using System.Text;
using System.Text.RegularExpressions;
public class FakeSsh {
    static string Enc(string value) { return Convert.ToBase64String(Encoding.UTF8.GetBytes(value)); }
    public static int Main(string[] args) {
        Console.InputEncoding = new UTF8Encoding(false);
        Console.OutputEncoding = new UTF8Encoding(false);
        string log = Environment.GetEnvironmentVariable("SURFXR_RELEASE_FIXTURE_LOG");
        if (args.Length > 0 && args[0] == "-S") {
            if (log != null) File.AppendAllText(log, "scp\n");
            return 0;
        }
        string command = args[args.Length - 1], target = args[args.Length - 2];
        string input = Console.In.ReadToEnd();
        if (log != null) File.AppendAllText(log, command + "\n");
        if (command == "fixture-empty") return 0;
        if (command == "fixture-invalid") { Console.WriteLine("fixture-invalid-json"); return 0; }
        if (command == "fixture-bad-status") { Console.WriteLine("{\"ok\":\"false\"}"); return 0; }
        if (command == "fixture-ssh-failed") return 42;
        if (command == "fixture-frame-failed") { Console.WriteLine("{\"ok\":false,\"error\":\"fixture_frame_rejection\"}"); return 1; }
        if (command == "fixture-false-success") { Console.WriteLine("{\"ok\":true}"); return 1; }
        if (command == "fixture-private-error") { Console.WriteLine("{\"ok\":false,\"error\":\"fixture-private-data\"}"); return 1; }
        if (command.StartsWith("name=$(hostname")) {
            // Dual-stack SSH defaults to a remote interface scope that Windows
            // cannot use. Only the identity query should request IPv4 explicitly.
            string ip = Array.IndexOf(args, "-4") >= 0 ? "192.0.2.1" : "fe80::1%wlanap";
            if (Environment.GetEnvironmentVariable("SURFXR_RELEASE_FIXTURE_PAIRING_FAILURE") != null) ip = "";
            Console.WriteLine("{\"ok\":true,\"hostname\":\"frame-fixture\",\"ip\":\"" + ip + "\"}");
        } else if (command.StartsWith("umask 077;")) {
            string id = Regex.Match(command, @"releases/([a-f0-9]{32})").Groups[1].Value;
            Console.WriteLine("{\"ok\":true,\"root\":\"/home/fixture/.cache/surfxr-client/releases/" + id + "\"}");
        } else if (command == "bash -s -- purge" || command == "bash -s -- uninstall") {
            if (!input.Contains("flock -n") || !input.Contains("--deploy")) return 43;
            Console.WriteLine("{\"ok\":true,\"uninstalled\":true,\"configuration_preserved\":" + (command.EndsWith(" purge") ? "false" : "true") + "}");
        } else if (command.EndsWith(" --deploy verify-release") && Environment.GetEnvironmentVariable("SURFXR_RELEASE_FIXTURE_MISSING_LIBRARY") != null) {
            Console.WriteLine("{\"ok\":true,\"missing_libraries\":[\"libavcodec.so.60\"]}");
        } else if (command.EndsWith(" --deploy install") && Environment.GetEnvironmentVariable("SURFXR_RELEASE_FIXTURE_DASHBOARD_FAILURE") != null) {
            Console.WriteLine("{\"ok\":true,\"installed\":true,\"backend\":\"surfxr\",\"dashboard_started\":false,\"dashboard_error\":\"frame_dashboard_service_start_failed\"}");
        } else {
            Console.WriteLine("{\"ok\":true,\"backend\":\"surfxr\",\"steamvr_restart_required\":true,\"missing_libraries\":[],\"dashboard_started\":true,\"target_base64\":\"" + Enc(target) +
                              "\",\"command_base64\":\"" + Enc(command) + "\",\"input_base64\":\"" + Enc(input) + "\"}");
        }
        return 0;
    }
}
'@
    $sourcePath = Join-Path $fixtureRoot 'fake transport.cs'
    [System.IO.File]::WriteAllText($sourcePath, $fixtureSource)
    $compileScript = Join-Path $fixtureRoot 'compile fixture.ps1'
    [System.IO.File]::WriteAllText($compileScript, 'param($Source, $Output); $ErrorActionPreference="Stop"; Add-Type -Path $Source -OutputAssembly $Output -OutputType ConsoleApplication')
    & "$env:SystemRoot/System32/WindowsPowerShell/v1.0/powershell.exe" -NoProfile -ExecutionPolicy Bypass -File $compileScript $sourcePath $selected.Ssh
    if ($LASTEXITCODE -ne 0) { throw 'Could not compile the isolated Windows argv consumer' }
    function Decode-Fixture([string]$Value) { return [System.Text.Encoding]::UTF8.GetString([Convert]::FromBase64String($Value)) }
    $quotedCommand = 'printf ''{"ok":true,"text":"with spaces"}\n''; root="$HOME/a b"; printf ''%s'' "a\"b"' + "`n" + 'tail\\'
    $inputFixture = '{"text":"test-' + [char]0x5e10 + [char]0x53f7 + '","path":"C:\\a b\\"}'
    $rpc = Invoke-ReleaseSsh fixture@offline $quotedCommand $inputFixture $selected.Ssh
    Assert-Check ((Decode-Fixture $rpc.target_base64) -ceq 'fixture@offline') 'SSH target roundtrip'
    Assert-Check ((Decode-Fixture $rpc.command_base64) -ceq $quotedCommand) 'quotes, backslashes, spaces and newline survive Windows argv'
    Assert-Check ((Decode-Fixture $rpc.input_base64) -ceq ($inputFixture + "`n")) 'UTF-8 fixture goes only through stdin'
    foreach ($failure in @(@('fixture-invalid','*invalid JSON*'), @('fixture-empty','*unexpected response*'),
                          @('fixture-bad-status','*unexpected response*'), @('fixture-ssh-failed','*exit code 42*'),
                          @('fixture-frame-failed','*fixture_frame_rejection*remote exit code 1*'),
                          @('fixture-false-success','*reported success but exited*'),
                          @('fixture-private-error','*frame_operation_failed*'))) {
        $rejected = $false
        try { $null = Invoke-ReleaseSsh fixture@offline $failure[0] '' $selected.Ssh }
        catch { $rejected = $_.Exception.Message -like $failure[1] }
        Assert-Check $rejected ("RPC failure explained: " + $failure[0])
    }

    # Menu 2 and 3 must each use exactly one SSH call and never invoke SCP.
    Copy-Item -LiteralPath $selected.Ssh -Destination $selected.Scp -Force
    foreach ($name in @('tools/windows-x64/openssh/ssh.exe','tools/windows-x64/openssh/scp.exe')) {
        $files[$name] = (Get-FileHash -LiteralPath (Join-Path $fixtureRoot $name) -Algorithm SHA256).Hash.ToLowerInvariant()
    }
    $manifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $manifestFile -Encoding UTF8
    $env:SURFXR_RELEASE_FIXTURE_LOG = Join-Path $fixtureRoot 'fake transport calls.txt'
    function Resolve-FrameReleaseTarget([string]$Target) { return $Target }
    function Read-Host([string]$Prompt) { if ($Prompt -ne 'Select [1]') { throw 'Unexpected installer prompt' }; return '2' }
    Invoke-FrameRelease -Directory $fixtureRoot -Action menu -Target fixture@offline -Python python -Link direct
    $calls = @(Get-Content -LiteralPath $env:SURFXR_RELEASE_FIXTURE_LOG)
    Assert-Check ($calls.Count -eq 1 -and $calls[0] -eq 'bash -s -- uninstall') 'menu 2 uses one SSH call without SCP or staging'
    Clear-Content -LiteralPath $env:SURFXR_RELEASE_FIXTURE_LOG
    function Read-Host([string]$Prompt) { if ($Prompt -ne 'Select [1]') { throw 'Unexpected installer prompt' }; return '3' }
    Invoke-FrameRelease -Directory $fixtureRoot -Action menu -Target fixture@offline -Python python -Link direct
    $calls = @(Get-Content -LiteralPath $env:SURFXR_RELEASE_FIXTURE_LOG)
    Assert-Check ($calls.Count -eq 1 -and $calls[0] -eq 'bash -s -- purge') 'menu 3 uses one SSH call without SCP, staging or account prompts'
    Clear-Content -LiteralPath $env:SURFXR_RELEASE_FIXTURE_LOG
    function Read-Host([string]$Prompt) { if ($Prompt -ne 'Select [1]') { throw 'Unexpected installer prompt' }; return '1' }
    $installMessages = @(Invoke-FrameRelease -Directory $fixtureRoot -Action menu -Target fixture@offline -Python python -Link direct 6>&1)
    Assert-Check (($installMessages | Out-String) -like '*Restart SteamVR on the headset*') 'installation reports headset restart for registered input driver'
    $calls = @(Get-Content -LiteralPath $env:SURFXR_RELEASE_FIXTURE_LOG)
    Assert-Check ($calls.Count -eq 6 -and $calls[0] -like 'umask 077;*' -and $calls[1] -eq 'scp' -and
                  $calls[3] -like '* --deploy verify-release' -and $calls[-1] -like '* --deploy install') 'menu 1 retains verified staging and install orchestration'
    $env:SURFXR_RELEASE_FIXTURE_DASHBOARD_FAILURE = '1'
    $warnings = @(Invoke-FrameRelease -Directory $fixtureRoot -Action install -Target fixture@offline -Python python -Link direct 3>&1)
    Assert-Check ($warnings.Count -eq 1 -and $warnings[0].ToString() -like '*frame_dashboard_service_start_failed*headset desktop*') 'installed client remains usable when dashboard launch reports a concrete failure'
    $env:SURFXR_RELEASE_FIXTURE_DASHBOARD_FAILURE = $previousDashboardFailure
    Clear-Content -LiteralPath $env:SURFXR_RELEASE_FIXTURE_LOG
    $manifest.install_profile = 'surfxr'
    $manifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $manifestFile -Encoding UTF8
    Invoke-FrameRelease -Directory $fixtureRoot -Action install -Target fixture@offline -Python python -Link direct
    $calls = @(Get-Content -LiteralPath $env:SURFXR_RELEASE_FIXTURE_LOG)
    Assert-Check ($calls.Count -eq 6 -and $calls[-1] -like '* --deploy install') 'SurfXR uses existing installer with SURFXR backend'
    Clear-Content -LiteralPath $env:SURFXR_RELEASE_FIXTURE_LOG
    $paired = @(Invoke-FrameRelease -Directory $fixtureRoot -Action install -Target fixture@offline -Python python -Link direct -PairOnInstall)
    $receipt = @($paired | Where-Object { $_ -is [string] -and $_.StartsWith('SURFXR_PAIR_ID ') })
    Assert-Check ($receipt.Count -eq 1) 'Dashboard installation returns one SSH-verified pairing receipt'
    $identity = $receipt[0].Substring(15) | ConvertFrom-Json
    Assert-Check ($identity.hostname -eq 'frame-fixture' -and $identity.ip -eq '192.0.2.1') 'pairing identity is from the SSH target'
    foreach ($invalid in @(@{hostname='frame'; ip='fe80::1%wlanap'}, @{hostname='frame'; ip='::1'},
        @{hostname='frame'; ip='127.0.0.1'}, @{hostname='frame'; ip='0.0.0.0'}, @{hostname='frame'; ip=''},
        @{hostname='bad/name'; ip='192.0.2.1'}, @{ip='192.0.2.1'}, @{hostname='frame'})) {
        $rejected = $false
        try { $null = ConvertTo-FramePairingReceipt ([pscustomobject]$invalid) } catch { $rejected = $true }
        Assert-Check $rejected 'invalid identity never emits a trust receipt'
    }
    $calls = @(Get-Content -LiteralPath $env:SURFXR_RELEASE_FIXTURE_LOG)
    Assert-Check ($calls.Count -eq 7 -and $calls[-1].StartsWith('name=$(hostname')) 'identity is read only after successful installation'
    $dashboardResult = Join-Path $fixtureRoot 'dashboard result.log'
    [System.IO.File]::WriteAllText($dashboardResult, '')
    Invoke-DashboardFrameRelease -ResultFile $dashboardResult -Directory $fixtureRoot -Target fixture@offline -Python python -Link direct -Action install -PairOnInstall
    $resultText = [System.IO.File]::ReadAllText($dashboardResult)
    Assert-Check ($resultText -like '*SURFXR_PAIR_ID*frame-fixture*' -and $resultText -like '*Restart SteamVR on the headset*') 'interactive console install also retains receipt and instructions'
    $failed = $false
    try { Invoke-DashboardFrameRelease -ResultFile $dashboardResult -Directory (Join-Path $fixtureRoot 'missing') -Target fixture@offline -Python python -Link direct -Action install -PairOnInstall }
    catch { $failed = $true }
    $resultText = [System.IO.File]::ReadAllText($dashboardResult)
    Assert-Check ($failed -and $resultText -like '*Installation failed:*' -and $resultText -notlike '*SURFXR_PAIR_ID*') 'failed retry replaces the result and never retains a stale pairing receipt'
    Clear-Content -LiteralPath $env:SURFXR_RELEASE_FIXTURE_LOG
    $commandPlan = (Invoke-FrameRelease -Directory $fixtureRoot -Action install -Target fixture@offline -Python python -Link direct -PairOnInstall -Plan) -join "`n"
    Assert-Check ((Get-Item -LiteralPath $env:SURFXR_RELEASE_FIXTURE_LOG).Length -eq 0) 'pairing plan never contacts Frame'
    Assert-Check ($commandPlan.Contains('umask 077;') -and $commandPlan.Contains(' --deploy verify-release') -and
        $commandPlan.Contains(' --deploy inspect') -and $commandPlan.Contains(' --deploy install') -and
        $commandPlan.Contains('name=$(hostname') -and $commandPlan.Contains('"link":"direct"') -and
        $commandPlan.Contains($selected.Scp) -and $commandPlan.Contains('${REMOTE_ROOT}')) 'Plan contains shell, transfer, native commands, JSON and explicit dynamic root'
    $previewId = [regex]::Match($commandPlan, 'releases/([a-f0-9]{32})').Groups[1].Value
    $expectedCommands = New-FrameReleaseCommands $fixtureRoot 'fixture@offline' direct $selected $previewId '${REMOTE_ROOT}'
    foreach ($name in @('Prepare','Permission','Verify','Inspect','Install','Request','Identity')) {
        Assert-Check ($commandPlan.Contains($expectedCommands.$name)) ("Preview contains executor command exactly: $name")
    }
    Assert-Check (([regex]::Matches($commandPlan, "'-4'")).Count -eq 1 -and
        $commandPlan.Contains((Format-ReleaseCommand $selected.Ssh (Get-ReleaseSshArguments 'fixture@offline' $expectedCommands.Identity -Ipv4)))) 'only pairing preview requests IPv4, matching execution'
    Clear-Content -LiteralPath $env:SURFXR_RELEASE_FIXTURE_LOG
    $env:SURFXR_RELEASE_FIXTURE_MISSING_LIBRARY = '1'
    Invoke-FrameRelease -Directory $fixtureRoot -Action install -Target fixture@offline -Python python -Link direct
    $calls = @(Get-Content -LiteralPath $env:SURFXR_RELEASE_FIXTURE_LOG)
    Assert-Check ($calls.Count -eq 6 -and $calls[3] -like '* --deploy verify-release' -and $calls[-1] -like '* --deploy install') 'runtime library metadata does not block installation'
    $env:SURFXR_RELEASE_FIXTURE_MISSING_LIBRARY = $previousMissingLibrary
    $env:SURFXR_RELEASE_FIXTURE_LOG = $previousFixtureLog

    # New PC adapter with an independently versioned ARM64 package. Exercise the
    # real root parameter binder in Windows PowerShell, using only fake SSH/SCP.
    $pcAdapter = Join-Path $fixtureRoot 'pc-app/frame-installer'
    foreach ($name in @('Install-Frame.ps1','launch/Start-Frame.ps1',
                       'launch/frame_release.ps1','launch/frame_release_commands.ps1')) {
        $destination = Join-Path $pcAdapter $name
        New-Item -ItemType Directory -Path (Split-Path $destination) -Force | Out-Null
        Copy-Item -LiteralPath (Join-Path $PSScriptRoot "../$name") -Destination $destination
    }
    $entry = Join-Path $pcAdapter 'Install-Frame.ps1'
    $shell = "$env:SystemRoot/System32/WindowsPowerShell/v1.0/powershell.exe"
    $bundledPlan = & $shell -NoProfile -File $entry -Action install -ReleaseDirectory $fixtureRoot -FrameSsh fixture@localhost -PairOnInstall -Plan
    Assert-Check ($LASTEXITCODE -eq 0 -and ($bundledPlan -join ' ').Contains(' --deploy install')) 'packaged PC adapter accepts Dashboard plan arguments'
    & $shell -NoProfile -File $entry -Action install -ReleaseDirectory $fixtureRoot -FrameSsh fixture@localhost -PairOnInstall -DashboardResult $dashboardResult
    Assert-Check ($LASTEXITCODE -eq 0 -and [IO.File]::ReadAllText($dashboardResult).Contains('SURFXR_PAIR_ID')) 'packaged PC adapter installs and pairs through original transport'
    $env:SURFXR_RELEASE_FIXTURE_PAIRING_FAILURE = '1'
    $savedPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        & $shell -NoProfile -File $entry -Action install -ReleaseDirectory $fixtureRoot -FrameSsh fixture@localhost -PairOnInstall -DashboardResult $dashboardResult 2>$null
    } finally { $ErrorActionPreference = $savedPreference }
    $resultText = [IO.File]::ReadAllText($dashboardResult)
    Assert-Check ($LASTEXITCODE -ne 0 -and $resultText.Contains('installation completed, but automatic pairing failed:') -and
        $resultText.Contains('IPv4 pairing address') -and -not $resultText.Contains('SURFXR_PAIR_ID') -and
        ([regex]::Matches($resultText, 'Installation failed:')).Count -eq 1) 'pairing failure retains installation success, explains address and is written once'
    $env:SURFXR_RELEASE_FIXTURE_PAIRING_FAILURE = $previousPairingFailure
    & $shell -NoProfile -File $entry -Action uninstall -ReleaseDirectory $fixtureRoot -FrameSsh fixture@localhost -DashboardResult $dashboardResult
    $removalText = [IO.File]::ReadAllText($dashboardResult)
    Assert-Check ($LASTEXITCODE -eq 0 -and $removalText.Contains('Saved connections and settings were preserved.') -and
        -not $removalText.Contains('SURFXR_PAIR_ID')) 'Dashboard removal uses native uninstall and retains diagnostics without pairing'

    # A startup failure must be returned in the Dashboard result even before
    # the transport writer can be loaded.
    $transport = Join-Path $pcAdapter 'launch/Start-Frame.ps1'
    Move-Item -LiteralPath $transport -Destination ($transport + '.unavailable')
    [IO.File]::WriteAllText($dashboardResult, '')
    $savedPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        & $shell -NoProfile -File $entry -Action install -ReleaseDirectory $fixtureRoot -FrameSsh fixture@localhost -PairOnInstall -DashboardResult $dashboardResult 2>$null
    } finally { $ErrorActionPreference = $savedPreference }
    Assert-Check ($LASTEXITCODE -ne 0 -and [IO.File]::ReadAllText($dashboardResult).Contains('Installation failed:')) 'early startup errors reach Dashboard instead of a bare exit code'

    [System.IO.File]::WriteAllText($selected.Ssh, 'tampered-fixture')
    $rejected = $false
    try { $null = Assert-FrameRelease $fixtureRoot } catch { $rejected = $_.Exception.Message -like '*hash mismatch*' }
    Assert-Check $rejected 'tampered tool rejected before execution'
    Copy-Item -LiteralPath $selected.Scp -Destination $selected.Ssh -Force
    $validated.files.PSObject.Properties.Remove('tools/windows-x64/openssh/scp.exe')
    $rejected = $false
    try { $null = Get-ReleaseTools $fixtureRoot 'python' $validated } catch { $rejected = $_.Exception.Message -like '*Bundled tool is missing*' }
    Assert-Check $rejected 'incomplete portable package cannot use system fallback'

    function Invoke-ReleaseSsh { throw 'Plan unexpectedly attempted SSH' }
    function Read-Host { throw 'Plan unexpectedly prompted for input' }
    $plan = Invoke-FrameRelease -Directory $fixtureRoot -Action install -Target 'steamos@invalid-fixture' -Python python -Link direct -Plan
    Assert-Check (($plan -join ' ') -like '*Release install via steamos@invalid-fixture*' -and
                  ($plan -join ' ') -like '*--deploy install*streaming remains idle*') 'Plan emits actual commands without side effects'
    $entryPlan = & (Join-Path $PSScriptRoot '../Install-Frame.ps1') -ReleaseDirectory $fixtureRoot -Action install -Plan
    Assert-Check (($entryPlan -join ' ') -like '*Release install via steamos@frame*' -and
                  ($entryPlan -join ' ') -notlike '*Build/check*') 'root default directory enters release installation in PowerShell 5.1'
    $purgePlan = & (Join-Path $PSScriptRoot '../Install-Frame.ps1') -ReleaseDirectory $fixtureRoot -Action purge -Plan
    Assert-Check (($purgePlan -join ' ') -like '*Release purge via steamos@frame*' -and
                  ($purgePlan -join ' ') -like '*deletes the Frame IP memory*') 'root full uninstall parameter and Plan'
    foreach ($entry in @('Start-Frame.ps1','Start-FrameSingle.ps1')) {
        $forwarded = & (Join-Path $PSScriptRoot "../$entry") -ReleaseDirectory $fixtureRoot -ReleaseAction purge -Plan
        Assert-Check (($forwarded -join ' ') -like '*Release purge*') ("full uninstall forwarded by " + $entry)
    }
    Write-Output 'Production release checks passed: Windows argv/UTF-8, RPC errors, menus 1/2/3 (removal without SCP), tool hashes and Plan; no network or user services.'
} finally {
    $env:SURFXR_RELEASE_FIXTURE_PAIRING_FAILURE = $previousPairingFailure
    $env:SURFXR_RELEASE_FIXTURE_MISSING_LIBRARY = $previousMissingLibrary
    $env:SURFXR_RELEASE_FIXTURE_LOG = $previousFixtureLog
    $env:SURFXR_RELEASE_FIXTURE_DASHBOARD_FAILURE = $previousDashboardFailure
    $resolvedFixture = [System.IO.Path]::GetFullPath($fixtureRoot)
    if (-not $resolvedFixture.StartsWith($fixtureParent, [System.StringComparison]::OrdinalIgnoreCase) -or
        (Split-Path $resolvedFixture -Leaf) -notlike 'surfxr-frame-release-check-*') { throw 'Unsafe test cleanup path' }
    if (Test-Path -LiteralPath $resolvedFixture) { Remove-Item -LiteralPath $resolvedFixture -Recurse -Force }
}
