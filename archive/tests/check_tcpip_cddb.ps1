$m = Mount-DiskImage -ImagePath 'G:\csmw10.VHD' -PassThru
$parts = $m | Get-Disk | Get-Partition
$osPart = $parts | Where-Object { Test-Path "$($_.DriveLetter):\Windows\System32\config\SYSTEM" }
$sysPath = "$($osPart.DriveLetter):\Windows\System32\config\SYSTEM"

& reg load HKLM\OFFLINE_SYS "$sysPath"
Start-Sleep -Milliseconds 500

$out = @()
$out += "=== TCPIP PARAMETERS ==="
$out += (& reg query "HKLM\OFFLINE_SYS\ControlSet001\Services\Tcpip\Parameters" /s)

$out += "=== CRITICAL DEVICE DATABASE ==="
$out += (& reg query "HKLM\OFFLINE_SYS\ControlSet001\Control\CriticalDeviceDatabase" /s 2>&1)

$out += "=== DISK TIMEOUT VALUE ==="
$out += (& reg query "HKLM\OFFLINE_SYS\ControlSet001\Services\disk" /v "TimeOutValue" 2>&1)

[GC]::Collect()
[GC]::WaitForPendingFinalizers()
& reg unload HKLM\OFFLINE_SYS
Dismount-DiskImage -ImagePath 'G:\csmw10.VHD'

$out | Out-File -FilePath "C:\Users\Admin\Desktop\cobdiskless\tcpip_cddb_check.txt" -Encoding utf8
