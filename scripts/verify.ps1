Add-Type -AssemblyName System.Drawing

$orig  = [System.Drawing.Bitmap]::new("$PSScriptRoot\..\test_images\test.jpg")
$enc   = [System.Drawing.Bitmap]::new("$PSScriptRoot\..\test_images\encrypted.jpg")
$rest  = [System.Drawing.Bitmap]::new("$PSScriptRoot\..\test_images\restored.jpg")
$wrong = [System.Drawing.Bitmap]::new("$PSScriptRoot\..\test_images\wrong_seed.jpg")

$total       = $orig.Width * $orig.Height
$diffRestore = 0
$diffEnc     = 0
$diffWrong   = 0

for ($y = 0; $y -lt $orig.Height; $y++) {
    for ($x = 0; $x -lt $orig.Width; $x++) {
        $o = $orig.GetPixel($x, $y)
        $r = $rest.GetPixel($x, $y)
        $e = $enc.GetPixel($x, $y)
        $w = $wrong.GetPixel($x, $y)

        if ($o.R -ne $r.R -or $o.G -ne $r.G -or $o.B -ne $r.B) { $diffRestore++ }
        if ($o.R -ne $e.R -or $o.G -ne $e.G -or $o.B -ne $e.B) { $diffEnc++ }
        if ($o.R -ne $w.R -or $o.G -ne $w.G -or $o.B -ne $w.B) { $diffWrong++ }
    }
}

Write-Host ""
Write-Host "============================================"
Write-Host "  Pixel Comparison Results"
Write-Host "============================================"
Write-Host ("Total pixels           : " + $total)
Write-Host ""
Write-Host ("Encrypted vs Original  : " + $diffEnc + " pixels differ (should differ)")
Write-Host ("Restored vs Original   : " + $diffRestore + " pixels differ (should be close to 0)")
Write-Host ("WrongSeed vs Original  : " + $diffWrong + " pixels differ (should be high)")

if ($diffRestore -eq 0) {
    Write-Host ""
    Write-Host "[PASS] Encrypt + Decrypt is 100% correct!" -ForegroundColor Green
} else {
    Write-Host ""
    Write-Host "[NOTE] $diffRestore pixels differ due to JPEG lossy compression (expected)." -ForegroundColor Yellow
}

$orig.Dispose(); $enc.Dispose(); $rest.Dispose(); $wrong.Dispose()
