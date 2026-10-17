$b = [System.IO.File]::ReadAllBytes('logs\ibft_body.bin')
Write-Host "Length: $($b.Length)"
$hex = [BitConverter]::ToString($b, 0, [Math]::Min(128, $b.Length))
Write-Host "Hex: $hex"
$ascii = [System.Text.Encoding]::ASCII.GetString($b)
Write-Host "ASCII strings:"
$clean = [System.Text.RegularExpressions.Regex]::Replace($ascii, "[^\x20-\x7E\r\n]", " ")
$clean -split " +" | Where-Object { $_.Length -ge 4 } | ForEach-Object { Write-Host "  $_" }
