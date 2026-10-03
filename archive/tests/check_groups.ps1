$m = Mount-DiskImage -ImagePath 'G:\csmw10.VHD' -PassThru
$parts = $m | Get-Disk | Get-Partition
$osPart = $parts | Where-Object { Test-Path "$($_.DriveLetter):\Windows\System32" }
$dl = $osPart.DriveLetter

& reg load HKLM\OFFLINE_SYS "$dl`:\Windows\System32\config\SYSTEM"
$out = @()

$services = @("rt640x64", "ndis", "tcpip", "netbt", "tdx", "fwpkclnt", "iScsiPrt", "disk", "partmgr", "volmgr", "mountmgr", "fltmgr")
foreach ($s in $services) {
    $out += "=== Service: $s ==="
    $out += (& reg query "HKLM\OFFLINE_SYS\ControlSet001\Services\$s" 2>&1)
}

$out += "=== ServiceGroupOrder List ==="
$out += (& reg query "HKLM\OFFLINE_SYS\ControlSet001\Control\ServiceGroupOrder" /v "List" 2>&1)

$out += "=== GroupOrderList ==="
$out += (& reg query "HKLM\OFFLINE_SYS\ControlSet001\Control\GroupOrderList" 2>&1)

[GC]::Collect()
[GC]::WaitForPendingFinalizers()
& reg unload HKLM\OFFLINE_SYS
Dismount-DiskImage -ImagePath 'G:\csmw10.VHD'

$out | Out-File -FilePath "C:\Users\Admin\Desktop\cobdiskless\vhd_groups_check.txt" -Encoding utf8
