param([string]$Destination = (Join-Path $PSScriptRoot 'Assets'))
# Draws the TDM icon: a download arrow dropping into a tray on a blue tile, with a small progress bar.
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
New-Item -ItemType Directory -Path $Destination -Force | Out-Null

function New-IconBitmap([int]$size) {
    $bmp = New-Object Drawing.Bitmap($size, $size, [Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = [Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.Clear([Drawing.Color]::Transparent)
    $s = [single]$size
    $r = [single]([Math]::Max(2, $s * 0.22))
    $path = New-Object Drawing.Drawing2D.GraphicsPath
    $m = [single]($s * 0.04); $w = $s - 2 * $m
    $path.AddArc($m, $m, $r, $r, 180, 90); $path.AddArc($m + $w - $r, $m, $r, $r, 270, 90)
    $path.AddArc($m + $w - $r, $m + $w - $r, $r, $r, 0, 90); $path.AddArc($m, $m + $w - $r, $r, $r, 90, 90)
    $path.CloseFigure()
    $bg = New-Object Drawing.Drawing2D.LinearGradientBrush((New-Object Drawing.PointF(0, 0)), (New-Object Drawing.PointF(0, $s)), [Drawing.ColorTranslator]::FromHtml('#2F8CFF'), [Drawing.ColorTranslator]::FromHtml('#1450C8'))
    $g.FillPath($bg, $path)
    $white = New-Object Drawing.SolidBrush([Drawing.Color]::White)
    # Arrow: shaft and head.
    $cx = $s / 2
    $shaftW = [single]([Math]::Max(2, $s * 0.15))
    $g.FillRectangle($white, [single]($cx - $shaftW / 2), [single]($s * 0.17), $shaftW, [single]($s * 0.30))
    $head = @(
        (New-Object Drawing.PointF([single]($cx - $s * 0.24), [single]($s * 0.42))),
        (New-Object Drawing.PointF([single]($cx + $s * 0.24), [single]($s * 0.42))),
        (New-Object Drawing.PointF($cx, [single]($s * 0.66)))
    )
    $g.FillPolygon($white, $head)
    # Tray.
    $pen = New-Object Drawing.Pen([Drawing.Color]::White, [single]([Math]::Max(1.5, $s * 0.075)))
    $pen.StartCap = $pen.EndCap = [Drawing.Drawing2D.LineCap]::Round
    $pen.LineJoin = [Drawing.Drawing2D.LineJoin]::Round
    $ty = [single]($s * 0.80)
    $g.DrawLines($pen, @(
        (New-Object Drawing.PointF([single]($s * 0.20), [single]($s * 0.66))),
        (New-Object Drawing.PointF([single]($s * 0.20), $ty)),
        (New-Object Drawing.PointF([single]($s * 0.80), $ty)),
        (New-Object Drawing.PointF([single]($s * 0.80), [single]($s * 0.66)))
    ))
    if ($size -ge 32) {
        # Progress tick in the tray (green): the "manager" part.
        $green = New-Object Drawing.Pen([Drawing.ColorTranslator]::FromHtml('#6BE38B'), [single]($s * 0.05))
        $green.StartCap = $green.EndCap = [Drawing.Drawing2D.LineCap]::Round
        $g.DrawLine($green, [single]($s * 0.32), [single]($s * 0.735), [single]($s * 0.56), [single]($s * 0.735))
        $green.Dispose()
    }
    $pen.Dispose(); $white.Dispose(); $bg.Dispose(); $path.Dispose(); $g.Dispose()
    return $bmp
}

# Multi-resolution ICO with PNG-compressed entries (supported since Windows Vista).
$sizes = 16, 20, 24, 32, 40, 48, 64, 256
$images = foreach ($size in $sizes) {
    $bmp = New-IconBitmap $size
    $ms = New-Object IO.MemoryStream
    $bmp.Save($ms, [Drawing.Imaging.ImageFormat]::Png)
    if ($size -eq 256) { $bmp.Save((Join-Path $Destination 'logo-256.png'), [Drawing.Imaging.ImageFormat]::Png) }
    $bmp.Dispose()
    , $ms.ToArray()
}
$out = New-Object IO.MemoryStream
$bw = New-Object IO.BinaryWriter($out)
$bw.Write([UInt16]0); $bw.Write([UInt16]1); $bw.Write([UInt16]$sizes.Count)
$offset = 6 + 16 * $sizes.Count
for ($i = 0; $i -lt $sizes.Count; $i++) {
    $dim = if ($sizes[$i] -ge 256) { 0 } else { $sizes[$i] }
    $bw.Write([byte]$dim); $bw.Write([byte]$dim); $bw.Write([byte]0); $bw.Write([byte]0)
    $bw.Write([UInt16]1); $bw.Write([UInt16]32)
    $bw.Write([UInt32]$images[$i].Length); $bw.Write([UInt32]$offset)
    $offset += $images[$i].Length
}
foreach ($img in $images) { $bw.Write($img) }
$bw.Flush()
[IO.File]::WriteAllBytes((Join-Path $Destination 'app.ico'), $out.ToArray())
$bw.Dispose()
