$m = Mount-DiskImage -ImagePath 'G:\csmw10.VHD' -PassThru
$parts = $m | Get-Disk | Get-Partition
$osPart = $parts | Where-Object { Test-Path "$($_.DriveLetter):\Windows\System32\config\SYSTEM" }
$win = "$($osPart.DriveLetter):\Windows"

$out = @()
$oemInf = "$win\INF\oem0.inf"
if (Test-Path $oemInf) {
    $out += "=== OEM0.INF FOUND ==="
    $lines = Get-Content $oemInf | Select-String "8168"
    foreach ($l in $lines) { $out += $l.Line }
} else {
    $out += "oem0.inf NOT FOUND, searching for rt640x64 inf..."
    $infs = Get-ChildItem "$win\INF\*.inf" | Select-String "rt640x64" | Select-Object -First 5
    foreach ($i in $infs) { $out += "$($i.Path): $($i.Line)" }
}

Dismount-DiskImage -ImagePath 'G:\csmw10.VHD'
$out | Out-File -FilePath "C:\Users\Admin\Desktop\cobdiskless\oem_inf_check.txt" -Encoding utf8
