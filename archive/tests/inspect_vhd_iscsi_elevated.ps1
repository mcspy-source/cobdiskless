$m = Mount-DiskImage -ImagePath 'G:\csmw10.VHD' -PassThru
$parts = $m | Get-Disk | Get-Partition
$osPart = $parts | Where-Object { Test-Path "$($_.DriveLetter):\Windows\System32" }
$dl = $osPart.DriveLetter
$out = @()
$out += "OS Drive Letter: $dl"
$out += "msiscsi.sys: $(Test-Path "$dl`:\Windows\System32\drivers\msiscsi.sys")"
$out += "iscsiprt.sys: $(Test-Path "$dl`:\Windows\System32\drivers\iscsiprt.sys")"
$out += "iscsi.inf: $(Test-Path "$dl`:\Windows\INF\iscsi.inf")"
$out += "iscsidsc.dll: $(Test-Path "$dl`:\Windows\System32\iscsidsc.dll")"
$out += "iscsiexe.dll: $(Test-Path "$dl`:\Windows\System32\iscsiexe.dll")"
$out += "iscsicli.exe: $(Test-Path "$dl`:\Windows\System32\iscsicli.exe")"

& reg load HKLM\OFFLINE_SYS "$dl`:\Windows\System32\config\SYSTEM"
$out += "=== SELECT ==="
$out += (& reg query "HKLM\OFFLINE_SYS\Select" 2>&1)

$out += "=== ISCSIPRT IN CONTROLSET001 ==="
$out += (& reg query "HKLM\OFFLINE_SYS\ControlSet001\Services\iScsiPrt" /s 2>&1)

$out += "=== MSISCSI IN CONTROLSET001 ==="
$out += (& reg query "HKLM\OFFLINE_SYS\ControlSet001\Services\msiscsi" /s 2>&1)

$out += "=== ROOT\\ISCSIPRT IN ENUM ==="
$out += (& reg query "HKLM\OFFLINE_SYS\ControlSet001\Enum\ROOT\ISCSIPRT" /s 2>&1)

$out += "=== CRITICAL DEVICE DATABASE ISCSIPRT ==="
$out += (& reg query "HKLM\OFFLINE_SYS\ControlSet001\Control\CriticalDeviceDatabase\root#iscsiprt" /s 2>&1)

$out += "=== SCSI CLASS ALL ==="
$out += (& reg query "HKLM\OFFLINE_SYS\ControlSet001\Control\Class\{4d36e97b-e325-11ce-bfc1-08002be10318}" 2>&1)

[GC]::Collect()
[GC]::WaitForPendingFinalizers()
& reg unload HKLM\OFFLINE_SYS
Dismount-DiskImage -ImagePath 'G:\csmw10.VHD'

$out | Out-File -FilePath "C:\Users\Admin\Desktop\cobdiskless\vhd_iscsi_status.txt" -Encoding utf8
