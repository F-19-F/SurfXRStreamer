[CmdletBinding()]
param(
    [ValidateSet('Release', 'Debug')]
    [string]$Configuration = 'Release',
    [string]$BuildDirectory = '',
    [switch]$SurfxrDesktop,
    [switch]$SurfxrDaemon,
    [string]$SurfxrRoot = '',
    [string]$FFmpegRoot = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$taskSource = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../surfxr'))
$SurfxrDesktop = $true
$SurfxrDaemon = $true
if (-not $SurfxrRoot) { $SurfxrRoot = Join-Path $taskSource "pc/surfxr-server" }
if (-not $FFmpegRoot) { $FFmpegRoot = Join-Path $SurfxrRoot "deps/windows/ffmpeg" }
if (-not $BuildDirectory) { $BuildDirectory = Join-Path $taskSource '../build/pc/native' }
$taskBuild = [System.IO.Path]::GetFullPath($BuildDirectory)
$buildRoot = [System.IO.Path]::GetFullPath((Join-Path $taskSource '../build'))
if (-not $taskBuild.StartsWith($buildRoot + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'BuildDirectory must be inside the repository build directory.'
}
$taskCmake = (Get-Command cmake.exe -ErrorAction Stop).Source
$taskVswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $taskVswhere -PathType Leaf)) {
    throw 'Visual Studio 2022 C++ build tools are required for this Windows build.'
}
$taskVs = & $taskVswhere -latest -version '[17.0,18.0)' -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if ($LASTEXITCODE -ne 0 -or -not $taskVs) {
    throw 'Visual Studio 2022 with the C++ x64 tools was not found.'
}

# CMake selects the installed Windows SDK and invokes MSVC without requiring a
# developer shell. BCrypt and Winsock are OS libraries; no OpenSSL package is
$taskTarget = 'surfxr-daemon'
$taskOptions = @('-DSURFXR_ENABLE_SURFXR_DESKTOP=ON',"-DSURFXR_DESKTOP_FFMPEG_ROOT=$FFmpegRoot")
$taskOptions += "-DSURFXR_ENABLE_SURFXR_DAEMON=$(if ($SurfxrDaemon) { 'ON' } else { 'OFF' })"
if ($SurfxrRoot) { $taskOptions += "-DSURFXR_SURFXR_ROOT=$SurfxrRoot" }
& $taskCmake -S $taskSource -B $taskBuild -G 'Visual Studio 17 2022' -A x64 "-DCMAKE_GENERATOR_INSTANCE=$taskVs" @taskOptions | Out-Host
if ($LASTEXITCODE -ne 0) { throw "CMake configuration failed for $taskTarget." }
$taskTargets = @('surfxr-desktop', 'surfxr-desktop-module')
if ($SurfxrDaemon) { $taskTargets += 'surfxr-daemon' }
& $taskCmake --build $taskBuild --config $Configuration --target @taskTargets --parallel 2 | Out-Host
if ($LASTEXITCODE -ne 0) { throw "Build failed for $taskTarget." }
$taskBinary = Join-Path $taskBuild "$Configuration\$taskTarget.exe"
if (-not (Test-Path -LiteralPath $taskBinary -PathType Leaf)) {
    throw "The build completed without producing $taskTarget.exe."
}
Write-Output $taskBinary
