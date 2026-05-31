Add-Type -AssemblyName System.Drawing

$bmp = New-Object System.Drawing.Bitmap 400, 300
$g   = [System.Drawing.Graphics]::FromImage($bmp)
$g.Clear([System.Drawing.Color]::White)

$colors = @(
    [System.Drawing.Color]::Red,
    [System.Drawing.Color]::Blue,
    [System.Drawing.Color]::Green,
    [System.Drawing.Color]::Orange,
    [System.Drawing.Color]::Purple,
    [System.Drawing.Color]::Cyan,
    [System.Drawing.Color]::Magenta,
    [System.Drawing.Color]::Yellow
)

$rng = New-Object System.Random 42
for ($i = 0; $i -lt 30; $i++) {
    $x    = $rng.Next(0, 360)
    $y    = $rng.Next(0, 260)
    $r    = $rng.Next(20, 80)
    $col  = $colors[$rng.Next(0, $colors.Length)]
    $brush = New-Object System.Drawing.SolidBrush($col)
    $g.FillEllipse($brush, $x, $y, $r, $r)
    $brush.Dispose()
}

# วาดข้อความ
$font  = New-Object System.Drawing.Font("Arial", 20)
$brush = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::Black)
$g.DrawString("SECRET IMAGE 555", $font, $brush, 80, 130)
$font.Dispose()
$brush.Dispose()

$g.Dispose()

$jpegCodec = [System.Drawing.Imaging.ImageCodecInfo]::GetImageEncoders() | Where-Object { $_.MimeType -eq "image/jpeg" }
$encParams = New-Object System.Drawing.Imaging.EncoderParameters(1)
$encParams.Param[0] = New-Object System.Drawing.Imaging.EncoderParameter([System.Drawing.Imaging.Encoder]::Quality, 95L)

# Ensure directory exists
$targetDir = "$PSScriptRoot\..\test_images"
if (-not (Test-Path $targetDir)) {
    New-Item -ItemType Directory -Force -Path $targetDir | Out-Null
}

$bmp.Save("$targetDir\test.jpg", $jpegCodec, $encParams)
$bmp.Dispose()

Write-Host "Test image created: $targetDir\test.jpg (400x300)"
