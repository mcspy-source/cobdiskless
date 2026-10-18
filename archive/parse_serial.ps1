$raw = [System.IO.File]::ReadAllBytes('logs\qemu_serial_vm1.log')
Write-Host "Total bytes: $($raw.Length)"
$txt = [System.Text.Encoding]::ASCII.GetString($raw)
$clean = [System.Text.RegularExpressions.Regex]::Replace($txt, "[^\x20-\x7E\r\n]", " ")
$lines = $clean -split "`r?`n"
foreach ($line in $lines) {
    $trimmed = $line.Trim()
    if ($trimmed.Length -gt 0) {
        Write-Host $trimmed
    }
}
