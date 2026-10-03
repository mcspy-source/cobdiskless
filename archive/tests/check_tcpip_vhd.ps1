$ErrorActionPreference = 'Stop'
$m = Mount-DiskImage -ImagePath 'G:\csmw10.VHD' -PassThru
$parts = $m | Get-Disk | Get-Partition
$osPart = $parts | Where-Object { Test-Path "$($_.DriveLetter):\Windows\System32\config\SYSTEM" }
$sysPath = "$($osPart.DriveLetter):\Windows\System32\config\SYSTEM"

& reg load HKLM\OFFLINE_SYS "$sysPath"
Start-Sleep -Milliseconds 500

$out = @()
$out += "=== Tcpip Service in VHD ==="
$out += (& reg query "HKLM\OFFLINE_SYS\ControlSet001\Services\Tcpip" 2>&1)

$out += "=== NDIS Service in VHD ==="
$out += (& reg query "HKLM\OFFLINE_SYS\ControlSet001\Services\NDIS" 2>&1)

[GC]::Collect()
[GC]::WaitForPendingFinalizers()
& reg unload HKLM\OFFLINE_SYS
Dismount-DiskImage -ImagePath 'G:\csmw10.VHD'

$out | Out-File -FilePath "C:\Users\Admin\Desktop\cobdiskless\tcpip_vhd_check.txt" -Encoding utf8
