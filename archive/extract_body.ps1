$raw = [System.IO.File]::ReadAllBytes('logs\ibft_extracted.bin')
$body = New-Object byte[] ($raw.Length - 36)
[Array]::Copy($raw, 36, $body, 0, $body.Length)
[System.IO.File]::WriteAllBytes('logs\ibft_body.bin', $body)
Write-Host "Wrote logs\ibft_body.bin, bytes: $($body.Length)"
