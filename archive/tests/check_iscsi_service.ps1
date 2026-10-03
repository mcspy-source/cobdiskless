$m = Mount-DiskImage -ImagePath 'G:\csmw10.VHD' -PassThru
$parts = $m | Get-Disk | Get-Partition
$osPart = $parts | Where-Object { Test-Path "$($_.DriveLetter):\Windows\System32\config\SYSTEM" }
$sysPath = "$($osPart.DriveLetter):\Windows\System32\config\SYSTEM"

& reg load HKLM\OFFLINE_SYS "$sysPath"
Start-Sleep -Milliseconds 500

$out = @()
$out += "=== REG QUERY SERVICES MSISCSI ==="
$out += (& reg query "HKLM\OFFLINE_SYS\ControlSet001\Services\msiscsi" 2>&1)
$out += (& reg query "HKLM\OFFLINE_SYS\ControlSet001\Services\MsiSCSI" 2>&1)

$out += "=== REG QUERY SERVICES SEARCH ISCSI ==="
$allServices = Get-ChildItem "HKLM:\OFFLINE_SYS\ControlSet001\Services"
$iscsiServices = $allServices | Where-Object { $_.PSChildName -like "*iscsi*" }
foreach ($s in $iscsiServices) {
    $out += "Found service: $($s.PSChildName)"
    $out += (& reg query "$($s.Name)" 2>&1)
}

[GC]::Collect()
[GC]::WaitForPendingFinalizers()
& reg unload HKLM\OFFLINE_SYS
Dismount-DiskImage -ImagePath 'G:\csmw10.VHD'

$out | Out-File -FilePath "C:\Users\Admin\Desktop\cobdiskless\iscsi_service_dump.txt" -Encoding utf8
