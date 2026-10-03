$ErrorActionPreference = 'Stop'
$m = Mount-DiskImage -ImagePath 'G:\csmw10.VHD' -PassThru
$parts = $m | Get-Disk | Get-Partition
$osPart = $parts | Where-Object { Test-Path "$($_.DriveLetter):\Windows\System32\config\SYSTEM" }
$sysPath = "$($osPart.DriveLetter):\Windows\System32\config\SYSTEM"

& reg load HKLM\OFFLINE_SYS "$sysPath"
Start-Sleep -Milliseconds 500

$out = @()
$out += "=== rt640x64 Service ==="
$out += (& reg query "HKLM\OFFLINE_SYS\ControlSet001\Services\rt640x64" /s 2>&1)

$out += "=== Tcpip Parameters Interfaces ==="
$out += (& reg query "HKLM\OFFLINE_SYS\ControlSet001\Services\Tcpip\Parameters\Interfaces" /s 2>&1)

$out += "=== Session Manager BootExecute ==="
$out += (& reg query "HKLM\OFFLINE_SYS\ControlSet001\Control\Session Manager" /v "BootExecute" 2>&1)

$out += "=== Memory Management PagingFiles ==="
$out += (& reg query "HKLM\OFFLINE_SYS\ControlSet001\Control\Session Manager\Memory Management" /v "PagingFiles" 2>&1)

$out += "=== iScsiPrt Service ==="
$out += (& reg query "HKLM\OFFLINE_SYS\ControlSet001\Services\iScsiPrt" /s 2>&1)

$out += "=== ServiceGroupOrder List ==="
$out += (& reg query "HKLM\OFFLINE_SYS\ControlSet001\Control\ServiceGroupOrder" /v "List" 2>&1)

[GC]::Collect()
[GC]::WaitForPendingFinalizers()
& reg unload HKLM\OFFLINE_SYS
Dismount-DiskImage -ImagePath 'G:\csmw10.VHD'

$out | Out-File -FilePath "C:\Users\Admin\Desktop\cobdiskless\deep_inspect.txt" -Encoding utf8
