# Renders every OLED screen of the remote firmware to PNG (docs/images/oled) using the real firmware drawing code.
# Needs Visual Studio C++ tools (cl.exe) and the Adafruit GFX sources that PlatformIO downloads on the first build:
#   pio run -d FogMachineRemoteControl
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = (Resolve-Path (Join-Path $here '..\..')).Path
$gfx = Join-Path $root 'FogMachineRemoteControl\.pio\libdeps\xiao_esp32c3\Adafruit GFX Library'
if (-not (Test-Path $gfx)) { throw "Adafruit GFX not found. Run 'pio run -d FogMachineRemoteControl' once first." }
$out = Join-Path $here 'build'
$img = Join-Path $root 'docs\images\oled'
New-Item -ItemType Directory -Force -Path $out, $img | Out-Null

$vsPath = Get-ChildItem 'C:\Program Files\Microsoft Visual Studio\*\*', 'C:\Program Files (x86)\Microsoft Visual Studio\*\*' -Directory -ErrorAction SilentlyContinue |
    Where-Object { Test-Path (Join-Path $_.FullName 'VC\Auxiliary\Build\vcvars64.bat') } | Select-Object -First 1 -ExpandProperty FullName
if (-not $vsPath) { throw 'Visual Studio C++ tools (vcvars64.bat) not found.' }
$vcvars = Join-Path $vsPath 'VC\Auxiliary\Build\vcvars64.bat'

$src = Join-Path $root 'FogMachineRemoteControl\src'
$srcs = @(
    (Join-Path $here 'oled_preview.cpp'),
    (Join-Path $gfx 'Adafruit_GFX.cpp'),
    (Join-Path $src 'ui\Screens.cpp'),
    (Join-Path $src 'menu\MenuState.cpp'),
    (Join-Path $root 'lib\FogLink\FogProto.cpp')
) | ForEach-Object { '"' + $_ + '"' }
$inc = @((Join-Path $here 'shim'), $src, (Join-Path $root 'lib\FogLink'), $gfx) | ForEach-Object { '/I"' + $_ + '"' }
$cmd = "`"$vcvars`" >nul && cl /nologo /std:c++17 /EHsc /O1 /DARDUINO=10800 /DESP32 /wd4244 /wd4267 /wd4305 $($inc -join ' ') /Fo`"$out\\`" /Fe`"$out\oled_preview.exe`" $($srcs -join ' ') && `"$out\oled_preview.exe`" `"$img`""
cmd /c $cmd
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
$py = (Get-Command python -ErrorAction SilentlyContinue)
if ($py) { & python (Join-Path $here 'compress_png.py') $img } else { Write-Warning 'python not found: PNGs stay uncompressed (~1.3 MB each)' }
