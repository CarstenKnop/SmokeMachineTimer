# Builds both firmwares with PlatformIO and stages the images where Fog Console finds them:
#
#   PCDiagnostics\FogConsole\firmware\timer\   firmware.bin  bootloader.bin  partitions.bin  boot_app0.bin
#   PCDiagnostics\FogConsole\firmware\remote\  (the same four files)
#
# A normal "dotnet build" / "dotnet publish" of Fog Console then copies the folder next to FogConsole.exe, and the
# Firmware page lists both as "Installed with the app". Without this step the page still finds the PlatformIO build
# folders when the tool is started from a source checkout, and you can always pick a file or folder by hand.
#
#   powershell -ExecutionPolicy Bypass -File tools\package-firmware.ps1            # build + stage
#   powershell -ExecutionPolicy Bypass -File tools\package-firmware.ps1 -NoBuild    # stage what is already built
#   powershell -ExecutionPolicy Bypass -File tools\package-firmware.ps1 -Destination D:\FogConsole\firmware
param(
    [switch]$NoBuild,
    [string]$Environment = "xiao_esp32c3",
    [string]$Destination = ""
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
if ($Destination -eq "") { $Destination = Join-Path $root "PCDiagnostics\FogConsole\firmware" }

function Find-PlatformIO {
    $cmd = Get-Command pio -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    $core = if ($env:PLATFORMIO_CORE_DIR) { $env:PLATFORMIO_CORE_DIR } else { Join-Path $env:USERPROFILE ".platformio" }
    $p = Join-Path $core "penv\Scripts\pio.exe"
    if (Test-Path $p) { return $p }
    throw "PlatformIO (pio) not found. Install the PlatformIO IDE extension or 'pip install platformio'."
}

function Find-BootApp0 {
    $core = if ($env:PLATFORMIO_CORE_DIR) { $env:PLATFORMIO_CORE_DIR } else { Join-Path $env:USERPROFILE ".platformio" }
    $p = Join-Path $core "packages\framework-arduinoespressif32\tools\partitions\boot_app0.bin"
    if (Test-Path $p) { return $p }
    return $null
}

$projects = @(
    @{ Name = "timer";  Dir = "FogMachineTimer" },
    @{ Name = "remote"; Dir = "FogMachineRemoteControl" }
)

if (-not $NoBuild) {
    $pio = Find-PlatformIO
    foreach ($p in $projects) {
        Write-Host "Building $($p.Dir) ..." -ForegroundColor Cyan
        & $pio run -d (Join-Path $root $p.Dir) -e $Environment
        if ($LASTEXITCODE -ne 0) { throw "Build of $($p.Dir) failed" }
    }
}

$bootApp0 = Find-BootApp0
foreach ($p in $projects) {
    $build = Join-Path $root "$($p.Dir)\.pio\build\$Environment"
    $app = Join-Path $build "firmware.bin"
    if (-not (Test-Path $app)) { throw "$app not found - build first (omit -NoBuild)" }

    $out = Join-Path $Destination $p.Name
    New-Item -ItemType Directory -Force $out | Out-Null
    foreach ($f in @("firmware.bin", "bootloader.bin", "partitions.bin")) {
        Copy-Item (Join-Path $build $f) (Join-Path $out $f) -Force
    }
    if ($bootApp0) { Copy-Item $bootApp0 (Join-Path $out "boot_app0.bin") -Force }
    else { Write-Warning "boot_app0.bin not found in the PlatformIO packages; a Reinstall leaves the OTA data erased (harmless)." }

    # which firmware is it? (the identity string every image carries, see lib/FogLink/FogIdent.h)
    $text = [System.Text.Encoding]::ASCII.GetString([System.IO.File]::ReadAllBytes($app))
    $m = [regex]::Match($text, "FOGFW\|(timer|remote)\|([0-9A-Za-z.+-]+)\|")
    $what = if ($m.Success) { "$($m.Groups[1].Value) $($m.Groups[2].Value)" } else { "NO IDENTITY MARKER" }
    "{0,-7} {1,-18} {2,9:N0} bytes  ->  {3}" -f $p.Name, $what, (Get-Item $app).Length, $out
}

Write-Host ""
Write-Host "Staged in $Destination. Rebuild Fog Console (dotnet build PCDiagnostics\FogConsole.sln -c Release) to copy it next to FogConsole.exe." -ForegroundColor Green
