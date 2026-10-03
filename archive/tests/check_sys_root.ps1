$m = Mount-DiskImage -ImagePath 'G:\csmw10.VHD' -PassThru
$parts = $m | Get-Disk | Get-Partition
$osPart = $parts | Where-Object { Test-Path "$($_.DriveLetter):\Windows\System32\config\SYSTEM" }
$sysPath = "$($osPart.DriveLetter):\Windows\System32\config\SYSTEM"
$res = @()
$res += "sysPath: $sysPath"
$res += (& reg load HKLM\OFFLINE_SYS "$sysPath" 2>&1)
$res += (& reg query "HKLM\OFFLINE_SYS" 2>&1)
& reg unload HKLM\OFFLINE_SYS
Dismount-DiskImage -ImagePath 'G:\csmw10.VHD'
$res | Out-File -FilePath "C:\Users\Admin\Desktop\cobdiskless\sys_root_check.txt" -Encoding utf8
