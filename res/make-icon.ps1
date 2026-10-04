# Generates res/displays-1.ico to res/displays-4.ico, each showing that many
# coloured displays side by side. The tray icon uses the one that matches how many
# displays are connected (four is as many as stay legible at 16 pixels), and
# displays-3.ico doubles as the exe's own icon. Run from anywhere:
#   powershell -ExecutionPolicy Bypass -File res\make-icon.ps1
Add-Type -AssemblyName System.Drawing
$ErrorActionPreference = 'Stop'

# 20 and 24 are the tray sizes at 125% and 150% scaling. Without them Windows
# shrinks the 32 pixel image, which blurs edges this small.
$sizes  = @(16, 20, 24, 32, 48)
$outDir = Split-Path -Parent $MyInvocation.MyCommand.Path

$blue   = [System.Drawing.Color]::FromArgb(255,  56, 132, 240)
$green  = [System.Drawing.Color]::FromArgb(255,  60, 175,  95)
$purple = [System.Drawing.Color]::FromArgb(255, 150, 100, 220)
$orange = [System.Drawing.Color]::FromArgb(255, 240, 140,  40)

# Left to right. Two displays keep the original blue and orange pair.
#
# The keys are strings on purpose: an [ordered] dictionary indexed with an integer
# looks the entry up by position, not by key, so $palettes[1] would be the second
# palette and $palettes[4] would be nothing at all.
$palettes = [ordered]@{
    '1' = @($blue)
    '2' = @($blue, $orange)
    '3' = @($blue, $green, $orange)
    '4' = @($blue, $green, $purple, $orange)
}

function Get-Pixels([int]$S, $colors) {
    $n   = $colors.Count
    $bmp = New-Object System.Drawing.Bitmap($S, $S, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g   = [System.Drawing.Graphics]::FromImage($bmp)
    $g.Clear([System.Drawing.Color]::Transparent)

    # Whole pixels throughout - [Math]::Floor rather than [int], which rounds
    # halves to even - so every edge is crisp instead of anti-aliased to a blur.
    $m = [Math]::Max(1, [Math]::Floor($S * 0.07))                 # gap between displays
    $w = [Math]::Floor(($S - ($n + 1) * $m) / $n)
    $h = [Math]::Min([Math]::Max(3, [Math]::Floor($w * 0.74)), $S - 2 * $m)
    $x = [Math]::Floor(($S - ($n * $w + ($n - 1) * $m)) / 2)       # centre the row
    $y = [Math]::Floor(($S - $h) / 2)

    for ($i = 0; $i -lt $n; $i++) {
        $b  = New-Object System.Drawing.SolidBrush $colors[$i]
        $dx = $x + $i * ($w + $m)
        $g.FillRectangle($b, $dx, $y, $w, $h)

        # Little stands, only where there are pixels to spare.
        if ($S -ge 32) {
            $sw = [Math]::Max(2, [Math]::Floor($w * 0.34))
            $sh = [Math]::Max(1, [Math]::Floor($S * 0.05))
            $g.FillRectangle($b, ($dx + [Math]::Floor(($w - $sw) / 2)), ($y + $h), $sw, $sh)
        }
        $b.Dispose()
    }
    $g.Dispose()

    $rect = New-Object System.Drawing.Rectangle(0, 0, $S, $S)
    $data = $bmp.LockBits($rect, [System.Drawing.Imaging.ImageLockMode]::ReadOnly,
                          [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $buf = New-Object byte[] ($data.Stride * $S)
    [System.Runtime.InteropServices.Marshal]::Copy($data.Scan0, $buf, 0, $buf.Length)
    $bmp.UnlockBits($data)

    # GDI+ hands back top-down BGRA; an ICO's XOR bitmap is bottom-up.
    $flipped = New-Object byte[] ($S * $S * 4)
    for ($row = 0; $row -lt $S; $row++) {
        [Array]::Copy($buf, ($S - 1 - $row) * $data.Stride, $flipped, $row * $S * 4, $S * 4)
    }
    $bmp.Dispose()
    return ,$flipped
}

function Write-Ico([string]$out, $colors) {
    $images = @()
    foreach ($s in $sizes) {
        $px = Get-Pixels $s $colors

        $ms = New-Object System.IO.MemoryStream
        $bw = New-Object System.IO.BinaryWriter $ms
        # BITMAPINFOHEADER - biHeight is doubled to cover the (unused) AND mask.
        $bw.Write([uint32]40); $bw.Write([int32]$s); $bw.Write([int32]($s * 2))
        $bw.Write([uint16]1);  $bw.Write([uint16]32); $bw.Write([uint32]0)
        $bw.Write([uint32]($s * $s * 4))
        $bw.Write([int32]0); $bw.Write([int32]0); $bw.Write([uint32]0); $bw.Write([uint32]0)
        $bw.Write($px)
        # AND mask: left at zero, since 32bpp icons are masked by their alpha channel.
        $maskRow = [Math]::Floor(($s + 31) / 32) * 4
        $bw.Write((New-Object byte[] ($maskRow * $s)))
        $bw.Flush()

        $images += ,@{ Size = $s; Bytes = $ms.ToArray() }
        $bw.Dispose(); $ms.Dispose()
    }

    $fs = [System.IO.File]::Create($out)
    $bw = New-Object System.IO.BinaryWriter $fs
    $bw.Write([uint16]0); $bw.Write([uint16]1); $bw.Write([uint16]$images.Count)

    $offset = 6 + 16 * $images.Count
    foreach ($img in $images) {
        $bw.Write([byte]$img.Size); $bw.Write([byte]$img.Size)
        $bw.Write([byte]0); $bw.Write([byte]0)
        $bw.Write([uint16]1); $bw.Write([uint16]32)
        $bw.Write([uint32]$img.Bytes.Length)
        $bw.Write([uint32]$offset)
        $offset += $img.Bytes.Length
    }
    foreach ($img in $images) { $bw.Write($img.Bytes) }
    $bw.Flush(); $bw.Dispose(); $fs.Dispose()

    Write-Host "wrote $out ($((Get-Item $out).Length) bytes, sizes: $($sizes -join ', '))"
}

foreach ($n in $palettes.Keys) {
    $colors = $palettes[$n]
    if ($colors.Count -ne [int]$n) {
        throw "palette '$n' has $($colors.Count) colours; expected $n"
    }
    Write-Ico (Join-Path $outDir "displays-$n.ico") $colors
}
