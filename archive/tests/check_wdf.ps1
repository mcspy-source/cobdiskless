$m = Mount-DiskImage -ImagePath 'G:\csmw10.VHD' -PassThru
$disk = $m | Get-Disk
$parts = $disk | Get-Partition
$part2 = $parts | Where-Object { $_.PartitionNumber -eq 2 }
$vol = Get-Volume | Where-Object { $_.FileSystemLabel -eq "os" }
$sysPath = "$($vol.DriveLetter):\Windows\System32\config\SYSTEM"
& reg load HKLM\OFFLINE_SYS "$sysPath"
$res = @()
$res += (& reg query "HKLM\OFFLINE_SYS\ControlSet001\Services\Wdf01000" 2>&1)
& reg unload HKLM\OFFLINE_SYS
Dismount-DiskImage -ImagePath 'G:\csmw10.VHD'
$res | Out-File -FilePath "C:\Users\Admin\Desktop\cobdiskless\wdf_check.txt" -Encoding utf8
