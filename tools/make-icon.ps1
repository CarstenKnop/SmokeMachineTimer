# Draws the Fog Console application icon (a cloud with radio waves on a blue rounded square, like the logo in the window's top bar)
# and writes it as a multi-size .ico, plus a 256 px preview for the documentation.
#
#   pwsh -NoProfile -File tools\make-icon.ps1
#
# Output: PCDiagnostics\FogConsole\Assets\FogConsole.ico   (referenced by FogConsole.csproj as ApplicationIcon)
#         PCDiagnostics\docs\app-icon.png
# Every size is drawn on its own at 4x and scaled down, so the small ones stay crisp; below 40 px the radio waves are left out.
Add-Type -AssemblyName System.Drawing

$root = Split-Path -Parent $PSScriptRoot
$icoPath = Join-Path $root 'PCDiagnostics\FogConsole\Assets\FogConsole.ico'
$pngPath = Join-Path $root 'PCDiagnostics\docs\app-icon.png'
New-Item -ItemType Directory -Force (Split-Path $icoPath) | Out-Null

function New-RoundedRect([float]$x, [float]$y, [float]$w, [float]$h, [float]$r) {
    $p = New-Object System.Drawing.Drawing2D.GraphicsPath
    $d = 2 * $r
    $p.AddArc($x, $y, $d, $d, 180, 90)
    $p.AddArc($x + $w - $d, $y, $d, $d, 270, 90)
    $p.AddArc($x + $w - $d, $y + $h - $d, $d, $d, 0, 90)
    $p.AddArc($x, $y + $h - $d, $d, $d, 90, 90)
    $p.CloseFigure()
    $p
}

function Get-CloudPath([float]$S, [double]$scale) {
    # the cloud in unit coordinates (0..1), scaled about the centre of the icon
    function U([double]$v) { [float]((0.5 + ($v - 0.5) * $scale) * $S) }
    function L([double]$v) { [float]($v * $scale * $S) }
    $p = New-Object System.Drawing.Drawing2D.GraphicsPath([System.Drawing.Drawing2D.FillMode]::Winding)
    $bar = New-RoundedRect (U 0.19) (U 0.50) (L 0.62) (L 0.22) (L 0.11)
    $p.AddPath($bar, $false)
    $p.AddEllipse((U 0.25), (U 0.37), (L 0.26), (L 0.26))      # left bump
    $p.AddEllipse((U 0.37), (U 0.24), (L 0.34), (L 0.34))      # big bump
    $p.AddEllipse((U 0.58), (U 0.38), (L 0.22), (L 0.22))      # right bump
    $p
}

function New-IconBitmap([int]$size) {
    $ss = 4
    $S = [float]($size * $ss)
    $bmp = New-Object System.Drawing.Bitmap([int]$S, [int]$S, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = 'AntiAlias'
    $g.PixelOffsetMode = 'HighQuality'
    $g.Clear([System.Drawing.Color]::Transparent)

    # the tile
    $m = [float](0.02 * $S)
    $tile = New-RoundedRect $m $m ($S - 2 * $m) ($S - 2 * $m) ([float](0.23 * $S))
    $grad = New-Object System.Drawing.Drawing2D.LinearGradientBrush(
        (New-Object System.Drawing.PointF(0, 0)), (New-Object System.Drawing.PointF(0, $S)),
        [System.Drawing.Color]::FromArgb(255, 0x45, 0xA9, 0xFF), [System.Drawing.Color]::FromArgb(255, 0x1A, 0x5C, 0xCF))
    $g.FillPath($grad, $tile)
    # a soft light along the top edge
    $g.SetClip($tile)
    $glow = New-Object System.Drawing.Drawing2D.LinearGradientBrush(
        (New-Object System.Drawing.PointF(0, 0)), (New-Object System.Drawing.PointF(0, [float](0.5 * $S))),
        [System.Drawing.Color]::FromArgb(60, 255, 255, 255), [System.Drawing.Color]::FromArgb(0, 255, 255, 255))
    $g.FillRectangle($glow, 0, 0, $S, [float](0.5 * $S))
    $g.ResetClip()

    # the cloud, with a soft shadow below it
    $scale = if ($size -lt 40) { 1.18 } else { 1.0 }
    $cloud = Get-CloudPath $S $scale
    $shadow = $cloud.Clone()
    $mx = New-Object System.Drawing.Drawing2D.Matrix
    $mx.Translate(0, [float](0.022 * $S))
    $shadow.Transform($mx)
    $g.FillPath((New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(70, 6, 30, 90))), $shadow)
    $g.FillPath([System.Drawing.Brushes]::White, $cloud)

    # radio waves inside the cloud
    if ($size -ge 40) {
        $blue = [System.Drawing.Color]::FromArgb(255, 0x1F, 0x6B, 0xDF)
        $pen = New-Object System.Drawing.Pen($blue, [float](0.036 * $S))
        $pen.StartCap = 'Round'; $pen.EndCap = 'Round'
        $cx = [float](0.5 * $S); $cy = [float](0.665 * $S)
        foreach ($r in 0.085, 0.155) {
            $rr = [float]($r * $S)
            $g.DrawArc($pen, $cx - $rr, $cy - $rr, 2 * $rr, 2 * $rr, 225, 90)
        }
        $dot = [float](0.03 * $S)
        $g.FillEllipse((New-Object System.Drawing.SolidBrush($blue)), $cx - $dot, $cy - $dot, 2 * $dot, 2 * $dot)
    }
    $g.Dispose()

    $out = New-Object System.Drawing.Bitmap($size, $size, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $go = [System.Drawing.Graphics]::FromImage($out)
    $go.InterpolationMode = 'HighQualityBicubic'
    $go.SmoothingMode = 'HighQuality'
    $go.PixelOffsetMode = 'HighQuality'
    $go.CompositingMode = 'SourceCopy'
    $go.DrawImage($bmp, 0, 0, $size, $size)
    $go.Dispose(); $bmp.Dispose()
    $out
}

$sizes = 16, 20, 24, 32, 40, 48, 64, 96, 128, 256
$images = foreach ($s in $sizes) {
    $b = New-IconBitmap $s
    $ms = New-Object System.IO.MemoryStream
    $b.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    if ($s -eq 256) { $b.Save($pngPath, [System.Drawing.Imaging.ImageFormat]::Png) }
    $b.Dispose()
    [pscustomobject]@{ Size = $s; Data = $ms.ToArray() }
}

# the .ico container with PNG-compressed images
$fs = [System.IO.File]::Create($icoPath)
$bw = New-Object System.IO.BinaryWriter($fs)
$bw.Write([uint16]0); $bw.Write([uint16]1); $bw.Write([uint16]$images.Count)
$offset = 6 + 16 * $images.Count
foreach ($i in $images) {
    $dim = if ($i.Size -ge 256) { 0 } else { $i.Size }
    $bw.Write([byte]$dim); $bw.Write([byte]$dim); $bw.Write([byte]0); $bw.Write([byte]0)
    $bw.Write([uint16]1); $bw.Write([uint16]32)
    $bw.Write([uint32]$i.Data.Length); $bw.Write([uint32]$offset)
    $offset += $i.Data.Length
}
foreach ($i in $images) { $bw.Write($i.Data) }
$bw.Dispose(); $fs.Dispose()
"wrote $icoPath ($([math]::Round((Get-Item $icoPath).Length / 1KB, 1)) KB, sizes: $($sizes -join ', '))"
"wrote $pngPath"
