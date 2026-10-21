# Builds and runs the host-side FogLink tests with MSVC (Visual Studio 2022/2026 "Desktop development with C++").
# Usage:  powershell -File tools/native-tests/run.ps1
#
# Produces in tools/native-tests/build:
#   fog_sim_tests.exe   the protocol simulation (run here)
#   ota_peer.exe        the timer's over-the-air update receiver as a program; Fog Console's tests drive it (not run here)
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = Resolve-Path (Join-Path $here '..\..')
$lib = Join-Path $root 'lib\FogLink'
$out = Join-Path $here 'build'
New-Item -ItemType Directory -Force -Path $out | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $out 'peer') | Out-Null

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vsPath = $null
if (Test-Path $vswhere) { $vsPath = & $vswhere -latest -prerelease -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath }
if (-not $vsPath) { $vsPath = Get-ChildItem 'C:\Program Files\Microsoft Visual Studio\*\*' -Directory | Where-Object { Test-Path (Join-Path $_.FullName 'VC\Auxiliary\Build\vcvars64.bat') } | Select-Object -First 1 -ExpandProperty FullName }
if (-not $vsPath) { throw 'Visual Studio C++ tools not found (need cl.exe).' }
$vcvars = Join-Path $vsPath 'VC\Auxiliary\Build\vcvars64.bat'

function Lib-Sources([string[]]$names) { Get-ChildItem $lib -Filter *.cpp | Where-Object { $_.Name -in $names } | ForEach-Object { '"' + $_.FullName + '"' } }

$simSources = @('"' + (Join-Path $here 'fog_sim_tests.cpp') + '"') + (Lib-Sources 'FogProto.cpp', 'FogCycle.cpp', 'FogTimerNode.cpp', 'FogRemoteLink.cpp', 'FogOta.cpp', 'FogSha256.cpp')
$peerSources = @('"' + (Join-Path $here 'ota_peer.cpp') + '"') + (Lib-Sources 'FogOta.cpp', 'FogSha256.cpp')
$flags = "/nologo /std:c++17 /W4 /EHsc /O2 /I`"$lib`" /I`"$here`""

$cmd = "`"$vcvars`" >nul && cd /d `"$here`"" +
       " && cl $flags /Fo`"$out\peer\\`" /Fe`"$out\ota_peer.exe`" $($peerSources -join ' ')" +
       " && cl $flags /Fo`"$out\\`" /Fe`"$out\fog_sim_tests.exe`" $($simSources -join ' ')" +
       " && `"$out\fog_sim_tests.exe`""
cmd /c $cmd
exit $LASTEXITCODE
