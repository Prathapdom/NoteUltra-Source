param([string]$Destination = (Join-Path $PSScriptRoot 'Assets'))
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
New-Item -ItemType Directory -Path $Destination -Force | Out-Null
foreach ($entry in @(@('StoreLogo.png',50),@('Square44x44Logo.png',44),@('Square150x150Logo.png',150))) {
    $size=[int]$entry[1]; $bitmap=New-Object Drawing.Bitmap($size,$size); $g=[Drawing.Graphics]::FromImage($bitmap)
    $g.SmoothingMode=[Drawing.Drawing2D.SmoothingMode]::AntiAlias; $g.Clear([Drawing.ColorTranslator]::FromHtml('#161B25'))
    $pen=New-Object Drawing.Pen([Drawing.ColorTranslator]::FromHtml('#6ADFC1'),[single]($size*.065))
    $g.DrawLine($pen,[single]($size*.24),[single]($size*.77),[single]($size*.24),[single]($size*.23))
    $g.DrawLine($pen,[single]($size*.24),[single]($size*.23),[single]($size*.70),[single]($size*.77))
    $g.DrawLine($pen,[single]($size*.70),[single]($size*.77),[single]($size*.70),[single]($size*.23))
    $bitmap.Save((Join-Path $Destination $entry[0]),[Drawing.Imaging.ImageFormat]::Png)
    if ($size -eq 44) { $icon=[Drawing.Icon]::FromHandle($bitmap.GetHicon()); $stream=[IO.File]::Create((Join-Path $Destination 'app.ico')); try { $icon.Save($stream) } finally { $stream.Dispose(); $icon.Dispose() } }
    $pen.Dispose();$g.Dispose();$bitmap.Dispose()
}
