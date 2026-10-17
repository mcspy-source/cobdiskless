$bytes = [System.IO.File]::ReadAllBytes('logs\low_mem.bin')
$off = 0x9E640
$sig = [System.Text.Encoding]::ASCII.GetString($bytes, $off, 4)
$len = [BitConverter]::ToUInt32($bytes, $off + 4)
Write-Host "Sig: $sig, Len: $len"
$ibft = New-Object byte[] $len
[Array]::Copy($bytes, $off, $ibft, 0, $len)
[System.IO.File]::WriteAllBytes('logs\ibft_extracted.bin', $ibft)
Write-Host "Saved logs\ibft_extracted.bin ($len bytes)"
