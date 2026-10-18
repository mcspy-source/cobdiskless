$raw = [System.IO.File]::ReadAllBytes("logs\qemu_serial_vm3.log")
$sb = New-Object System.Text.StringBuilder
foreach ($b in $raw) {
    if (($b -ge 32 -and $b -le 126) -or $b -eq 10 -or $b -eq 13) {
        [void]$sb.Append([char]$b)
    }
}
[System.IO.File]::WriteAllText("logs\serial_clean_vm3.txt", $sb.ToString())
Get-Content "logs\serial_clean_vm3.txt" | Where-Object { $_.Trim().Length -gt 0 } | Select-Object -First 30
