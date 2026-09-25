# Generates res/app.ico: two coloured displays side by side, the left/right pair
# whose contents WinSwapper exchanges. Run from anywhere:
#   powershell -ExecutionPolicy Bypass -File res\make-icon.ps1
Add-Type -AssemblyName System.Drawing

$sizes  = @(16, 32, 48)
$outDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$out    = Join-Path $outDir 'app.ico'

$left  = [System.Drawing.Color]::FromArgb(255,  56, 132, 240)   # blue
$right = [System.Drawing.Color]::FromArgb(255, 240, 140,  40)   # orange

function Get-Pixels([int]$S) {
    $bmp = New-Object System.Drawing.Bitmap($S, $S, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g   = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.Clear([System.Drawing.Color]::Transparent)

    $m = [Math]::Max(1, [int]($S * 0.07))
    $w = [int](($S - 3 * $m) / 2)
    $h = [Math]::Max(3, [int]($w * 0.74))
    $y = [int](($S - $h) / 2)

    $b1 = New-Object System.Drawing.SolidBrush $left
    $b2 = New-Object System.Drawing.SolidBrush $right
    $g.FillRectangle($b1, $m, $y, $w, $h)
    $g.FillRectangle($b2, ($S - $m - $w), $y, $w, $h)

    # Little stands, only where there are pixels to spare.
    if ($S -ge 32) {
        $sw = [Math]::Max(2, [int]($w * 0.34))
        $sh = [Math]::Max(1, [int]($S * 0.05))
        $g.FillRectangle($b1, ($m + ($w - $sw) / 2), ($y + $h), $sw, $sh)
        $g.FillRectangle($b2, ($S - $m - $w + ($w - $sw) / 2), ($y + $h), $sw, $sh)
    }

    $b1.Dispose(); $b2.Dispose(); $g.Dispose()

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

$images = @()
foreach ($s in $sizes) {
    $px = Get-Pixels $s

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
