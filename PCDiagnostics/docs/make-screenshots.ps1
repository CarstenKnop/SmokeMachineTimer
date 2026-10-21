# Regenerates the README screenshots (docs\*-dark.png and docs\*-light.png).
# The simulator is seeded, so the pictures are reproducible. Run from anywhere:
#   powershell -ExecutionPolicy Bypass -File PCDiagnostics\docs\make-screenshots.ps1
# Needs a built app (dotnet build PCDiagnostics\FogConsole.sln -c Release).
param(
    [string]$Configuration = "Release",
    [string]$Only = ""          # e.g. -Only firmware: regenerate just the pictures whose name contains this text
)

$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root "FogConsole\bin\$Configuration\net10.0-windows\FogConsole.exe"
if (-not (Test-Path $exe)) { throw "Build first: $exe not found" }

# page, file prefix, extra arguments, seconds to let the virtual time run before the shot
$shots = @(
    @("Dashboard",   "dashboard",   "--paired 10 --no-chaos --action showcase", 22),
    @("Device",      "device",      "--paired 10 --no-chaos --select 0 --action device", 45),
    @("Pairing",     "pairing",     "--action discover",                         8),
    @("Channels",    "channels",    "--action survey",                           8),
    @("Diagnostics", "diagnostics", "--no-chaos",                               22),
    @("Console",     "console",     "--action console",                          3),
    @("Firmware",    "firmware",    "--paired 10 --no-chaos --action firmware",   8),
    @("Firmware",    "firmware-ota", "--paired 10 --no-chaos --action otaready",   10),
    @("Settings",    "settings",    "",                                          6)
)

foreach ($theme in @("dark", "light")) {
    foreach ($s in $shots) {
        if ($Only -ne "" -and $s[1] -notlike "*$Only*") { continue }
        $out = Join-Path $PSScriptRoot "$($s[1])-$theme.png"
        if (Test-Path $out) { Remove-Item $out -Force }
        $argList = @("--sim", "--seed", "42", "--page", $s[0], "--theme", $theme,
                     "--shot", $out, "--shot-scale", "1", "--delay", "$($s[3])", "--exit")
        if ($s[2] -ne "") { $argList += $s[2].Split(" ") }
        $p = Start-Process -FilePath $exe -ArgumentList $argList -PassThru -Wait
        $kb = if (Test-Path $out) { [math]::Round((Get-Item $out).Length / 1KB) } else { -1 }
        "{0,-22} exit={1} {2} KB" -f (Split-Path -Leaf $out), $p.ExitCode, $kb
    }
}

# Extra situations used by docs\FOG_CONSOLE_MANUAL.md (dark theme only). name, page, arguments, seconds
$extras = @(
    @("start",           "Dashboard",   "NOSIM",                                              3),
    @("simpanel",        "Dashboard",   "--paired 10 --no-chaos --action simpanel",           10),
    @("fail",            "Dashboard",   "--paired 10 --no-chaos --action fail",               14),
    @("migrate",         "Channels",    "--action migrate",                                   10),
    @("linklab",         "Channels",    "--action linktest --scroll 700",                     14),
    @("diagnostics-low", "Diagnostics", "--no-chaos --scroll 1500",                           22),
    @("packets",         "Console",     "--action packets",                                    8),
    @("raw",             "Console",     "--action raw",                                        6),
    @("firmware-flashing", "Firmware",  "--paired 10 --no-chaos --action firmwareflash",       8),
    @("firmware-done",   "Firmware",    "--paired 10 --no-chaos --action firmwaredone",        10),
    @("firmware-ota-progress", "Firmware", "--paired 10 --no-chaos --action otaprogress --scroll 440",      10),
    @("firmware-ota-done", "Firmware",  "--paired 10 --no-chaos --action otadone --scroll 440",             10),
    @("firmware-ota-fail", "Firmware",  "--paired 10 --no-chaos --action otafail --scroll 440",             10)
)
foreach ($s in $extras) {
    if ($Only -ne "" -and $s[0] -notlike "*$Only*") { continue }
    $out = Join-Path $PSScriptRoot "$($s[0])-dark.png"
    if (Test-Path $out) { Remove-Item $out -Force }
    $argList = @("--seed", "42", "--page", $s[1], "--theme", "dark", "--shot", $out, "--shot-scale", "1", "--delay", "$($s[3])", "--exit")
    if ($s[2] -eq "NOSIM") { $argList += "--no-settings" } else { $argList = @("--sim") + $argList; $argList += $s[2].Split(" ") }
    $p = Start-Process -FilePath $exe -ArgumentList $argList -PassThru -Wait
    $kb = if (Test-Path $out) { [math]::Round((Get-Item $out).Length / 1KB) } else { -1 }
    "{0,-26} exit={1} {2} KB" -f (Split-Path -Leaf $out), $p.ExitCode, $kb
}
