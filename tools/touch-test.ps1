# Touch test for Fog Console: drives the real app with injected touch input and checks that
#   * every page scrolls when a finger drags it,
#   * a slider thumb follows a finger,
#   * the "Hold to fog" button is held while a finger rests on it and released when the finger lifts.
#
#   pwsh -NoProfile -File tools\touch-test.ps1                 # all tests (needs a Release build and a free screen: nobody may touch it meanwhile)
#   pwsh -NoProfile -File tools\touch-test.ps1 -Test pages     # pages | slider | hold
#
# Uses the offline demo (--sim), so no hardware is needed. Exit code 0 = everything passed.
# Notes on the injection technique (DPI, refresh rate, pitfalls): the comments below.
param(
    [ValidateSet("all", "pages", "slider", "hold")][string]$Test = "all",
    [string]$Exe = (Join-Path $PSScriptRoot "..\PCDiagnostics\FogConsole\bin\Release\net10.0-windows\FogConsole.exe")
)

# Injected contacts must be refreshed every <= ~15 ms or Windows drops them (errors 1460 / 87); positions are physical pixels.
Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes, System.Drawing

Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
using System.Threading;
public static class Touch
{
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int x, y; }
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int left, top, right, bottom; }
    [StructLayout(LayoutKind.Sequential)] public struct POINTER_INFO {
        public uint pointerType; public uint pointerId; public uint frameId; public uint pointerFlags; public IntPtr sourceDevice; public IntPtr hwndTarget;
        public POINT ptPixelLocation; public POINT ptHimetricLocation; public POINT ptPixelLocationRaw; public POINT ptHimetricLocationRaw;
        public uint dwTime; public uint historyCount; public int InputData; public uint dwKeyStates; public ulong PerformanceCount; public int ButtonChangeType; }
    [StructLayout(LayoutKind.Sequential)] public struct POINTER_TOUCH_INFO {
        public POINTER_INFO pointerInfo; public uint touchFlags; public uint touchMask; public RECT rcContact; public RECT rcContactRaw; public uint orientation; public uint pressure; }

    [DllImport("user32.dll", SetLastError = true)] static extern bool InitializeTouchInjection(uint maxCount, uint mode);
    [DllImport("user32.dll", SetLastError = true)] static extern bool InjectTouchInput(uint count, [In] POINTER_TOUCH_INFO[] contacts);
    [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr ctx);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);

    public static string Init() { return InitializeTouchInjection(1, 1) ? "ok" : "InitializeTouchInjection failed: " + Marshal.GetLastWin32Error(); }

    // phase: "down" | "move" | "up"
    public static string Send(int x, int y, string phase)
    {
        uint flags = phase == "down" ? 0x00010000u | 0x2u | 0x4u       // DOWN | INRANGE | INCONTACT
                   : phase == "move" ? 0x00020000u | 0x2u | 0x4u       // UPDATE | INRANGE | INCONTACT
                   : 0x00040000u;                                      // UP
        var c = new POINTER_TOUCH_INFO();
        c.pointerInfo.pointerType = 2;                                 // PT_TOUCH
        c.pointerInfo.pointerFlags = flags;
        c.pointerInfo.ptPixelLocation.x = x; c.pointerInfo.ptPixelLocation.y = y;
        c.touchMask = 1 | 2 | 4;                                       // contact area, orientation, pressure
        c.rcContact.left = x - 2; c.rcContact.right = x + 2; c.rcContact.top = y - 2; c.rcContact.bottom = y + 2;
        c.orientation = 90; c.pressure = 32000;
        return InjectTouchInput(1, new[] { c }) ? "ok" : "InjectTouchInput failed: " + Marshal.GetLastWin32Error();
    }

    static volatile bool holding; static volatile int hx, hy; static Thread holdThread;
    public static void StartHold(int x, int y)
    {
        hx = x; hy = y;
        Send(x, y, "down"); holding = true;
        holdThread = new Thread(() => { int k = 0; while (holding) { Send(hx + (k++ % 2), hy, "move"); Thread.Sleep(10); } }) { IsBackground = true };
        holdThread.Start();
    }
    public static void MoveHold(int x, int y) { hx = x; hy = y; }          // the finger slides to (x, y) while it stays down
    public static string StopHold(int x, int y) { holding = false; holdThread.Join(); return Send(x, y, "up"); }
}
"@

function Initialize-Touch {
    [void][Touch]::SetProcessDpiAwarenessContext([IntPtr](-4))       # per-monitor v2
    $r = [Touch]::Init()
    if ($r -ne "ok") { throw $r }
}

function Show-Window($process) { $process.Refresh(); [void][Touch]::SetForegroundWindow($process.MainWindowHandle); Start-Sleep -Milliseconds 600 }

function Get-WindowRect($process) { $r = New-Object Touch+RECT; [void][Touch]::GetWindowRect($process.MainWindowHandle, [ref]$r); $r }

function Invoke-TouchTap([int]$x, [int]$y) { [void][Touch]::Send($x, $y, "down"); Start-Sleep -Milliseconds 80; [void][Touch]::Send($x, $y, "up") }

# A finger moving from (x1,y1) to (x2,y2): steps x delayMs ~ 330 ms for the defaults.
function Invoke-TouchDrag([int]$x1, [int]$y1, [int]$x2, [int]$y2, [int]$steps = 24, [int]$delayMs = 14) {
    [void][Touch]::Send($x1, $y1, "down")
    for ($i = 1; $i -le $steps; $i++) {
        [void][Touch]::Send([int]($x1 + ($x2 - $x1) * $i / $steps), [int]($y1 + ($y2 - $y1) * $i / $steps), "move")
        Start-Sleep -Milliseconds $delayMs
    }
    [void][Touch]::Send($x2, $y2, "up")
}

function Start-TouchHold([int]$x, [int]$y) { [Touch]::StartHold($x, $y) }
function Move-TouchHold([int]$x, [int]$y) { [Touch]::MoveHold($x, $y) }
function Stop-TouchHold([int]$x, [int]$y) { [Touch]::StopHold($x, $y) }

# ---- UI Automation helpers ----------------------------------------------------------------------------------------------------------------
function Get-UiaWindow($process) { [System.Windows.Automation.AutomationElement]::FromHandle($process.MainWindowHandle) }

function Find-UiaAll($root, $condition) { $root.FindAll([System.Windows.Automation.TreeScope]::Descendants, $condition) }

function New-UiaProperty($property, $value) { New-Object System.Windows.Automation.PropertyCondition($property, $value) }

# Scrollable areas of the window (visible, vertically scrollable), most scrollable first: Element, Pattern, ViewSize (% of content that is visible)
function Get-ScrollAreas($process) {
    $win = Get-UiaWindow $process
    $cond = New-UiaProperty ([System.Windows.Automation.AutomationElement]::IsScrollPatternAvailableProperty) $true
    $list = foreach ($e in (Find-UiaAll $win $cond)) {
        $sp = $e.GetCurrentPattern([System.Windows.Automation.ScrollPattern]::Pattern)
        if ($sp.Current.VerticallyScrollable -and -not $e.Current.IsOffscreen) {
            [pscustomobject]@{ Element = $e; Pattern = $sp; ViewSize = $sp.Current.VerticalViewSize; Rect = $e.Current.BoundingRectangle }
        }
    }
    $list | Sort-Object ViewSize
}

function Get-Sliders($process) {
    $win = Get-UiaWindow $process
    Find-UiaAll $win (New-UiaProperty ([System.Windows.Automation.AutomationElement]::ControlTypeProperty) ([System.Windows.Automation.ControlType]::Slider))
}

function Save-WindowShot($process, [string]$path, [int]$height = 0) {
    $r = Get-WindowRect $process
    $w = $r.right - $r.left; $h = if ($height -gt 0) { $height } else { $r.bottom - $r.top }
    $bmp = New-Object System.Drawing.Bitmap($w, $h)
    $g = [System.Drawing.Graphics]::FromImage($bmp); $g.CopyFromScreen($r.left, $r.top, 0, 0, $bmp.Size); $g.Dispose()
    $bmp.Save($path); $bmp.Dispose()
    $path
}

$failures = 0
function Check([string]$what, [bool]$ok, [string]$detail = "") {
    if ($ok) { Write-Host ("  PASS  {0} {1}" -f $what, $detail) -ForegroundColor Green }
    else { Write-Host ("  FAIL  {0} {1}" -f $what, $detail) -ForegroundColor Red; $script:failures++ }
}

function Start-Fog([string]$page, [string[]]$extra = @()) {
    Get-Process FogConsole -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep -Milliseconds 500
    $args = @("--sim", "--seed", "42", "--paired", "10", "--no-chaos", "--no-settings", "--page", $page, "--size", "1300x900") + $extra
    $p = Start-Process -FilePath $Exe -ArgumentList $args -PassThru
    Start-Sleep -Seconds 6
    Show-Window $p
    return $p
}

if (-not (Test-Path $Exe)) { throw "Build first: dotnet build PCDiagnostics\FogConsole.sln -c Release   ($Exe not found)" }
Initialize-Touch

if ($Test -in @("all", "pages")) {
    Write-Host "Pages: a finger dragging up must scroll the page down"
    foreach ($page in @("Dashboard", "Device", "Pairing", "Channels", "Diagnostics", "Firmware", "Settings")) {
        $p = Start-Fog $page @("--select", "0")
        $area = (Get-ScrollAreas $p) | Select-Object -First 1
        if (-not $area) { Check $page $false "no scrollable area found"; Stop-Process -Id $p.Id -Force; continue }
        $r = $area.Rect; $x = [int]($r.X + $r.Width * 0.45)
        $before = $area.Pattern.Current.VerticalScrollPercent
        Invoke-TouchDrag $x ([int]($r.Y + $r.Height * 0.75)) $x ([int]($r.Y + $r.Height * 0.25)) 20 14
        Start-Sleep -Milliseconds 900
        $after = $area.Pattern.Current.VerticalScrollPercent
        Check $page ($after -gt $before + 1) ("({0:0.#} % -> {1:0.#} %)" -f $before, $after)
        # and back down. (On the dashboard such a drag may start on a hold-to-fog button, which keeps the finger for itself: not tested there.)
        if ($page -eq "Dashboard") { Stop-Process -Id $p.Id -Force; continue }
        Invoke-TouchDrag $x ([int]($r.Y + $r.Height * 0.30)) $x ([int]($r.Y + $r.Height * 0.85)) 20 14
        Start-Sleep -Milliseconds 900
        Check "$page (back)" ($area.Pattern.Current.VerticalScrollPercent -lt $after - 1) ("({0:0.#} % -> {1:0.#} %)" -f $after, $area.Pattern.Current.VerticalScrollPercent)
        Stop-Process -Id $p.Id -Force
    }
}

if ($Test -in @("all", "slider")) {
    Write-Host "Slider: dragging the thumb 200 px to the right must raise the value"
    $p = Start-Fog "Device" @("--select", "0")
    $sl = Get-Sliders $p | Select-Object -First 1
    if (-not $sl) { Check "slider" $false "no slider found" }
    else {
        $rv = $sl.GetCurrentPattern([System.Windows.Automation.RangeValuePattern]::Pattern)
        $br = $sl.Current.BoundingRectangle
        $v0 = $rv.Current.Value
        $thumbX = [int]($br.X + 8 + ($v0 - $rv.Current.Minimum) / ($rv.Current.Maximum - $rv.Current.Minimum) * ($br.Width - 16)); $y = [int]($br.Y + $br.Height / 2)
        Invoke-TouchDrag $thumbX $y ($thumbX + 200) $y 20 16
        Start-Sleep -Milliseconds 500
        Check "slider" ($rv.Current.Value -gt $v0 + 1) ("({0:0.#} -> {1:0.#})" -f $v0, $rv.Current.Value)
    }
    Stop-Process -Id $p.Id -Force
}

if ($Test -in @("all", "hold")) {
    Write-Host "Hold to fog: the red FORCE ON banner shows while a finger rests on the button and goes when it lifts"
    function Test-RedBanner($proc) {
        $shot = Join-Path $env:TEMP "fog-touch-shot.png"
        [void](Save-WindowShot $proc $shot 300)
        $bmp = [System.Drawing.Bitmap]::FromFile($shot); $red = 0
        for ($yy = 100; $yy -lt 280; $yy += 4) { for ($xx = 300; $xx -lt [Math]::Min(1400, $bmp.Width); $xx += 4) { $c = $bmp.GetPixel($xx, $yy); if ($c.R -gt 190 -and $c.G -lt 110 -and $c.B -lt 110) { $red++ } } }
        $bmp.Dispose()
        return $red -gt 300
    }
    $p = Start-Fog "Dashboard"
    $btn = Find-UiaAll (Get-UiaWindow $p) (New-UiaProperty ([System.Windows.Automation.AutomationElement]::ControlTypeProperty) ([System.Windows.Automation.ControlType]::Button)) |
        Where-Object { $_.Current.Name -match "Hold" -and -not $_.Current.IsOffscreen } | Select-Object -First 1
    if (-not $btn) { Check "hold button" $false "not found" }
    else {
        $br = $btn.Current.BoundingRectangle; $x = [int]($br.X + $br.Width / 2); $y = [int]($br.Y + $br.Height / 2)
        Check "before" (-not (Test-RedBanner $p)) "(no banner)"
        Start-TouchHold $x $y
        Start-Sleep -Milliseconds 1500
        Check "while holding" (Test-RedBanner $p) "(banner shown)"
        [void](Stop-TouchHold $x $y)
        Start-Sleep -Milliseconds 1500
        Check "after release" (-not (Test-RedBanner $p)) "(banner gone)"

        # a finger that slides away from the button ends the hold (a scroll drag that starts on the button must not keep fogging)
        Start-TouchHold $x $y
        Start-Sleep -Milliseconds 1200
        $held = Test-RedBanner $p
        Move-TouchHold ($x + 40) ($y - 160)                      # well over the 18 DIP limit
        Start-Sleep -Milliseconds 1200
        Check "slides off" ($held -and -not (Test-RedBanner $p)) "(fogging stops when the finger leaves the button)"
        [void](Stop-TouchHold ($x + 40) ($y - 160))
    }
    Stop-Process -Id $p.Id -Force
}

Write-Host ""
if ($failures -eq 0) { Write-Host "All touch tests passed." -ForegroundColor Green } else { Write-Host "$failures check(s) failed." -ForegroundColor Red }
exit ([int]($failures -ne 0))
