$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
& choco install nsis pkgconfiglite zip unzip -y --no-progress
if ($LASTEXITCODE) { throw 'Windows packaging tool preparation failed' }
$cache = Join-Path $root 'build/deps'
New-Item -ItemType Directory -Force $cache | Out-Null
$alias = Join-Path $root 'surfxr/pc/surfxr-server/deps'
if (!(Test-Path -LiteralPath $alias)) { New-Item -ItemType Junction -Path $alias -Target $cache | Out-Null }
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vs = & $vswhere -latest -version '[17.0,18.0)' -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if ($LASTEXITCODE -or !$vs) { throw 'Visual Studio 2022 C++ tools are required' }
$crt = Get-ChildItem (Join-Path $vs 'VC/Redist/MSVC') -Directory | Sort-Object Name -Descending |
    ForEach-Object { Join-Path $_.FullName 'x64/Microsoft.VC143.CRT' } | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
$nsis = Join-Path ${env:ProgramFiles(x86)} 'NSIS/makensis.exe'
$clang = Join-Path $env:ProgramFiles 'LLVM/bin'
if (!$crt -or !(Test-Path $nsis) -or !(Test-Path (Join-Path $clang 'libclang.dll'))) { throw 'Missing CRT, NSIS or libclang' }
@("SURFXR_MSVC_REDIST_DIR=$crt", "SURFXR_MAKENSIS=$nsis", "LIBCLANG_PATH=$clang") |
    Out-File -FilePath $env:GITHUB_ENV -Append -Encoding utf8
Join-Path $env:CARGO_HOME 'bin' | Out-File -FilePath $env:GITHUB_PATH -Append -Encoding utf8
