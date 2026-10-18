$bytes = [System.IO.File]::ReadAllBytes('logs\low_mem.bin')
$off = 0x9E640

# Control structure: offset 48 (0x30)
Write-Host "=== Raw Hex Dump of iBFT rows 24 to 40 ==="
for ($row = 24; $row -lt 40; $row++) {
    $lineOff = $off + ($row * 16)
    $hex = (0..15 | ForEach-Object { $bytes[$lineOff + $_].ToString('X2') }) -join ' '
    $chars = (0..15 | ForEach-Object { $b = $bytes[$lineOff + $_]; if ($b -ge 32 -and $b -le 126) { [char]$b } else { '.' } }) -join ''
    Write-Host ('{0:X4}: {1}  |{2}|' -f ($row * 16), $hex, $chars)
}
