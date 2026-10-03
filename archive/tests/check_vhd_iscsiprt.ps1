$m = Mount-DiskImage -ImagePath 'G:\csmw10.VHD' -PassThru
$parts = $m | Get-Disk | Get-Partition
$osPart = $parts | Where-Object { Test-Path "$($_.DriveLetter):\Windows\System32\config\SYSTEM" }
$sysPath = "$($osPart.DriveLetter):\Windows\System32\config\SYSTEM"

& reg load HKLM\OFFLINE_SYS "$sysPath"
Start-Sleep -Milliseconds 500

$out = @()
$out += "=== OFFLINE iScsiPrt ==="
$out += (& reg query "HKLM\OFFLINE_SYS\ControlSet001\Services\iScsiPrt" /s 2>&1)
$out += "=== OFFLINE Enum ROOT ISCSIPRT ==="
$out += (& reg query "HKLM\OFFLINE_SYS\ControlSet001\Enum\ROOT\ISCSIPRT" /s 2>&1)
$out += "=== OFFLINE msiscsi ==="
$out += (& reg query "HKLM\OFFLINE_SYS\ControlSet001\Services\msiscsi" /s 2>&1)

[GC]::Collect()
[GC]::WaitForPendingFinalizers()
& reg unload HKLM\OFFLINE_SYS
Dismount-DiskImage -ImagePath 'G:\csmw10.VHD'

$out | Out-File -FilePath "C:\Users\Admin\Desktop\cobdiskless\vhd_iscsiprt_check.txt" -Encoding utf8
