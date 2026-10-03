$m = Mount-DiskImage -ImagePath 'G:\csmw10.VHD' -PassThru
$parts = $m | Get-Disk | Get-Partition
$bootPart = $parts | Where-Object { Test-Path "$($_.DriveLetter):\Boot\BCD" }
$bcdPath = "$($bootPart.DriveLetter):\Boot\BCD"

& reg load HKLM\OFFLINE_BCD "$bcdPath"
Start-Sleep -Milliseconds 500

$out = @()
$out += "=== BCD OBJECTS ==="
$out += (& reg query "HKLM\OFFLINE_BCD\Objects\{8ef8a6b1-b875-11f1-826b-c774f3307f8b}\Elements" /s)

[GC]::Collect()
[GC]::WaitForPendingFinalizers()
& reg unload HKLM\OFFLINE_BCD
Dismount-DiskImage -ImagePath 'G:\csmw10.VHD'

$out | Out-File -FilePath "C:\Users\Admin\Desktop\cobdiskless\bcd_elements_dump.txt" -Encoding utf8
