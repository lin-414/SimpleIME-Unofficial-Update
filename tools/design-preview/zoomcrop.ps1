param([string]$Src, [string]$Out, [int]$X, [int]$Y, [int]$W, [int]$H, [int]$Z)
Add-Type -AssemblyName System.Drawing
$img = [System.Drawing.Bitmap]::FromFile($Src)
$crop = $img.Clone((New-Object System.Drawing.Rectangle($X, $Y, $W, $H)), $img.PixelFormat)
$big = New-Object System.Drawing.Bitmap(($W * $Z), ($H * $Z))
$g = [System.Drawing.Graphics]::FromImage($big)
$g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::NearestNeighbor
$g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::Half
$g.DrawImage($crop, 0, 0, ($W * $Z), ($H * $Z))
$big.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$g.Dispose(); $big.Dispose(); $crop.Dispose(); $img.Dispose()
Write-Output "saved $Out"
