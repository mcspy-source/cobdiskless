$m = Mount-DiskImage -ImagePath 'G:\csmw10.VHD' -PassThru
$parts = $m | Get-Disk | Get-Partition
$bootPart = $parts | Where-Object { Test-Path "$($_.DriveLetter):\Boot\BCD" }
$bcdPath = "$($bootPart.DriveLetter):\Boot\BCD"

$out = & bcdedit /store "$bcdPath" /enum all /v
$out | Out-File -FilePath "C:\Users\Admin\Desktop\cobdiskless\bcd_verbose.txt" -Encoding utf8

Dismount-DiskImage -ImagePath 'G:\csmw10.VHD'
