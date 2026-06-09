# Genera B:\MIXER\src\mixer.ico  icona multi-risoluzione (PNG-in-ICO)
# Design: sfondo scuro arrotondato, scritta "MIXER" in alto, 2 fader sotto.
Add-Type -AssemblyName System.Drawing

$sizes = @(256, 64, 48, 32, 16)
$pngs  = @()

function New-RoundedPath([single]$x,[single]$y,[single]$w,[single]$h,[single]$r) {
    $p = New-Object System.Drawing.Drawing2D.GraphicsPath
    if ($r -lt 1) { $p.AddRectangle((New-Object System.Drawing.RectangleF($x,$y,$w,$h))); return $p }
    $d = $r * 2
    $p.AddArc($x, $y, $d, $d, 180, 90)
    $p.AddArc($x + $w - $d, $y, $d, $d, 270, 90)
    $p.AddArc($x + $w - $d, $y + $h - $d, $d, $d, 0, 90)
    $p.AddArc($x, $y + $h - $d, $d, $d, 90, 90)
    $p.CloseFigure()
    return $p
}

foreach ($S in $sizes) {
    $bmp = New-Object System.Drawing.Bitmap($S, $S, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode     = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::AntiAliasGridFit
    $g.Clear([System.Drawing.Color]::Transparent)

    # Sfondo arrotondato
    $bgRad = [single]([Math]::Max(2, $S * 0.18))
    $bgPath = New-RoundedPath 0 0 ([single]$S) ([single]$S) $bgRad
    $bgBrush = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 24, 27, 34))
    $g.FillPath($bgBrush, $bgPath)
    $borderPen = New-Object System.Drawing.Pen ([System.Drawing.Color]::FromArgb(255, 70, 74, 84)), ([single]([Math]::Max(1, $S*0.02)))
    $g.DrawPath($borderPen, $bgPath)

    # Scritta "MIXER" in alto (saltata sotto i 32px: illeggibile, lasciamo i fader)
    if ($S -ge 32) {
        $fontSize = [single]($S * 0.20)
        $font = New-Object System.Drawing.Font("Segoe UI", $fontSize, [System.Drawing.FontStyle]::Bold, [System.Drawing.GraphicsUnit]::Pixel)
        $sf = New-Object System.Drawing.StringFormat
        $sf.Alignment = [System.Drawing.StringAlignment]::Center
        $sf.LineAlignment = [System.Drawing.StringAlignment]::Center
        $txtRect = New-Object System.Drawing.RectangleF(0, [single]($S*0.06), [single]$S, [single]($S*0.28))
        $txtBrush = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 235, 238, 245))
        $g.DrawString("MIXER", $font, $txtBrush, $txtRect, $sf)
    }

    # Area fader
    $y0frac = 0.18
    if ($S -ge 32) { $y0frac = 0.40 }
    $y0 = [single]($S * $y0frac)
    $y1 = [single]($S * 0.88)
    $faderH = $y1 - $y0
    $trackW = [single]([Math]::Max(2, $S * 0.055))

    $cols = @(
        @{ x = 0.32; knobY = 0.32; c = [System.Drawing.Color]::FromArgb(255, 80, 205, 115) },  # verde
        @{ x = 0.62; knobY = 0.60; c = [System.Drawing.Color]::FromArgb(255, 245, 170, 70)  }   # arancio
    )
    foreach ($f in $cols) {
        $cx = [single]($S * $f.x)
        # Traccia
        $trackPath = New-RoundedPath ([single]($cx - $trackW/2)) $y0 $trackW $faderH ([single]($trackW/2))
        $trackBrush = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 64, 68, 78))
        $g.FillPath($trackBrush, $trackPath)
        # Manopola (fader knob)
        $knobW = [single]([Math]::Max(4, $S * 0.24))
        $knobH = [single]([Math]::Max(3, $S * 0.11))
        $knobCy = $y0 + $faderH * [single]$f.knobY
        $knobPath = New-RoundedPath ([single]($cx - $knobW/2)) ([single]($knobCy - $knobH/2)) $knobW $knobH ([single]([Math]::Max(1, $knobH*0.35)))
        $knobBrush = New-Object System.Drawing.SolidBrush $f.c
        $g.FillPath($knobBrush, $knobPath)
    }

    $g.Dispose()
    $ms = New-Object System.IO.MemoryStream
    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    $pngs += ,@{ size = $S; bytes = $ms.ToArray() }
    $ms.Dispose()
}

# Scrivi container .ICO con entry PNG (supportate da Windows Vista+)
$out = "B:\MIXER\src\mixer.ico"
$fs = New-Object System.IO.FileStream($out, [System.IO.FileMode]::Create)
$bw = New-Object System.IO.BinaryWriter($fs)
$bw.Write([UInt16]0)            # idReserved
$bw.Write([UInt16]1)            # idType = icon
$bw.Write([UInt16]$pngs.Count)  # idCount
$offset = 6 + (16 * $pngs.Count)
foreach ($p in $pngs) {
    $sz = $p.size
    $bw.Write([Byte]($(if ($sz -ge 256) { 0 } else { $sz })))  # width
    $bw.Write([Byte]($(if ($sz -ge 256) { 0 } else { $sz })))  # height
    $bw.Write([Byte]0)   # color count
    $bw.Write([Byte]0)   # reserved
    $bw.Write([UInt16]1) # planes
    $bw.Write([UInt16]32)# bit count
    $bw.Write([UInt32]$p.bytes.Length)
    $bw.Write([UInt32]$offset)
    $offset += $p.bytes.Length
}
foreach ($p in $pngs) { $bw.Write($p.bytes) }
$bw.Flush(); $bw.Close(); $fs.Close()
Write-Output "Creato $out  ($((Get-Item $out).Length) byte, $($pngs.Count) risoluzioni)"
