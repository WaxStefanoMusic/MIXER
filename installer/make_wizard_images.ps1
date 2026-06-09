# ============================================================================
#  Genera le immagini del wizard di Inno Setup ad alta risoluzione, cosi'
#  l'installer resta nitido su schermi 4K / DPI elevati (Setup scala da queste
#  verso il basso a 100/125/150/200%).
#
#    installer\wizard_large.png   banner laterale (Welcome/Finished)  164:314
#    installer\wizard_small.png   icona in alto a destra              quadrata
#
#  Uso:  powershell -ExecutionPolicy Bypass -File installer\make_wizard_images.ps1
# ============================================================================
Add-Type -AssemblyName System.Drawing

$here = Split-Path -Parent $MyInvocation.MyCommand.Path

# Palette coerente con l'icona (mixer_imgui dark theme)
$colBg     = [System.Drawing.Color]::FromArgb(255, 24, 27, 34)   # #181B22
$colBg2    = [System.Drawing.Color]::FromArgb(255, 16, 18, 23)   # piu' scuro (gradiente)
$colBorder = [System.Drawing.Color]::FromArgb(255, 70, 74, 84)   # #464A54
$colText   = [System.Drawing.Color]::FromArgb(255, 235, 238, 244)
$colSub    = [System.Drawing.Color]::FromArgb(255, 150, 156, 168)
$colAccent = [System.Drawing.Color]::FromArgb(255, 92, 203, 92)  # verde "audio attivo"
$colTrack  = [System.Drawing.Color]::FromArgb(255, 48, 52, 62)

function Save-Png([System.Drawing.Bitmap]$bmp, [string]$path) {
    $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
    Write-Host "scritto: $path  ($($bmp.Width)x$($bmp.Height))"
}

# ── Immagine grande: 164:314, generata a 4x (656x1256) per nitidezza a 4K ────
function New-WizardLarge {
    $W = 656; $H = 1256
    $bmp = New-Object System.Drawing.Bitmap($W, $H, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode     = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::AntiAliasGridFit

    # Sfondo a gradiente verticale
    $rect = New-Object System.Drawing.Rectangle(0, 0, $W, $H)
    $grad = New-Object System.Drawing.Drawing2D.LinearGradientBrush($rect, $colBg, $colBg2, 90.0)
    $g.FillRectangle($grad, $rect)

    # Titolo "MIXER"
    $titleSize = [single]($W * 0.165)
    $fTitle = New-Object System.Drawing.Font("Segoe UI", $titleSize, [System.Drawing.FontStyle]::Bold, [System.Drawing.GraphicsUnit]::Pixel)
    $sf = New-Object System.Drawing.StringFormat
    $sf.Alignment = [System.Drawing.StringAlignment]::Center
    $brText = New-Object System.Drawing.SolidBrush $colText
    $g.DrawString("MIXER", $fTitle, $brText, (New-Object System.Drawing.RectangleF(0, [single]($H*0.085), [single]$W, [single]($titleSize*1.3))), $sf)

    # Sottotitolo
    $subSize = [single]($W * 0.052)
    $fSub = New-Object System.Drawing.Font("Segoe UI", $subSize, [System.Drawing.FontStyle]::Regular, [System.Drawing.GraphicsUnit]::Pixel)
    $brSub = New-Object System.Drawing.SolidBrush $colSub
    $g.DrawString("Mixer audio per Windows", $fSub, $brSub, (New-Object System.Drawing.RectangleF(0, [single]($H*0.205), [single]$W, [single]($subSize*1.5))), $sf)

    # Tre fader stilizzati (richiamo all'icona) nella meta' bassa
    $faderCount = 3
    $areaTop = [single]($H * 0.36)
    $areaH   = [single]($H * 0.50)
    $slotW   = [single]($W / ($faderCount + 1))
    $trackW  = [single]($W * 0.018)
    $capW    = [single]($W * 0.085)
    $capH    = [single]($W * 0.05)
    $levels  = @(0.35, 0.62, 0.48)  # posizioni cap (0=basso, 1=alto)
    for ($i = 0; $i -lt $faderCount; $i++) {
        $cx = [single]($slotW * ($i + 1))
        # binario
        $tx = [single]($cx - $trackW/2)
        $trackPath = New-Object System.Drawing.Drawing2D.GraphicsPath
        $trackPath.AddRectangle((New-Object System.Drawing.RectangleF($tx, $areaTop, $trackW, $areaH)))
        $brTrack = New-Object System.Drawing.SolidBrush $colTrack
        $g.FillPath($brTrack, $trackPath)
        # porzione attiva (dal cap in giu')
        $capY = [single]($areaTop + $areaH * (1.0 - $levels[$i]) - $capH/2)
        $activeY = [single]($capY + $capH/2)
        $brAcc = New-Object System.Drawing.SolidBrush $colAccent
        $g.FillRectangle($brAcc, $tx, $activeY, $trackW, [single]($areaTop + $areaH - $activeY))
        # cap del fader
        $g.FillRectangle($brAcc, [single]($cx - $capW/2), $capY, $capW, $capH)
    }

    Save-Png $bmp (Join-Path $here "wizard_large.png")
    $g.Dispose(); $bmp.Dispose()
}

# ── Immagine piccola: quadrata, riusa il disegno dell'icona gia' presente ────
function New-WizardSmall {
    $src = Join-Path $here "icon_preview.png"
    if (-not (Test-Path $src)) { Write-Host "icon_preview.png assente: salto la small"; return }
    # 192x192 e' abbondante per qualsiasi DPI (la modern small e' ~55x58 logici)
    $S = 192
    $img = [System.Drawing.Image]::FromFile($src)
    $bmp = New-Object System.Drawing.Bitmap($S, $S, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $g.SmoothingMode     = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.Clear([System.Drawing.Color]::Transparent)
    $g.DrawImage($img, 0, 0, $S, $S)
    Save-Png $bmp (Join-Path $here "wizard_small.png")
    $g.Dispose(); $bmp.Dispose(); $img.Dispose()
}

New-WizardLarge
New-WizardSmall
Write-Host "Fatto."
