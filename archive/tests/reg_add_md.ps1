$m = Mount-DiskImage -ImagePath 'G:\csmw10.VHD' -PassThru
$parts = $m | Get-Disk | Get-Partition
$osPart = $parts | Where-Object { Test-Path "$($_.DriveLetter):\Windows\System32\config\SYSTEM" }
$sysPath = "$($osPart.DriveLetter):\Windows\System32\config\SYSTEM"

& reg load HKLM\OFFLINE_SYS "$sysPath"
Start-Sleep -Milliseconds 500

& reg add "HKLM\OFFLINE_SYS\MountedDevices" /v "\DosDevices\C:" /t REG_BINARY /d "8984123E0000602200000000" /f
& reg add "HKLM\OFFLINE_SYS\MountedDevices" /v "\??\Volume{3e128489-0000-0000-0000-602200000000}" /t REG_BINARY /d "8984123E0000602200000000" /f
& reg add "HKLM\OFFLINE_SYS\MountedDevices" /v "\??\Volume{3e128489-0000-0000-0000-100000000000}" /t REG_BINARY /d "8984123E0000100000000000" /f

# Query to confirm reg add worked
$q = & reg query "HKLM\OFFLINE_SYS\MountedDevices"
$q | Out-File -FilePath "C:\Users\Admin\Desktop\cobdiskless\reg_query_md.txt" -Encoding utf8

[GC]::Collect()
[GC]::WaitForPendingFinalizers()
& reg unload HKLM\OFFLINE_SYS
Dismount-DiskImage -ImagePath 'G:\csmw10.VHD'
