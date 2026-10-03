$m = Mount-DiskImage -ImagePath 'G:\csmw10.VHD' -PassThru
$parts = $m | Get-Disk | Get-Partition
$osPart = $parts | Where-Object { Test-Path "$($_.DriveLetter):\Windows\System32\config\SYSTEM" }
$sysPath = "$($osPart.DriveLetter):\Windows\System32\config\SYSTEM"

& reg load HKLM\OFFLINE_SYS "$sysPath"
Start-Sleep -Milliseconds 500

$out = @()
foreach ($svc in @("disk", "partmgr", "mountmgr", "volmgr", "storvsc", "storahci", "stornvme", "spaceport")) {
    $key = "HKLM:\OFFLINE_SYS\ControlSet001\Services\$svc"
    if (Test-Path $key) {
        $p = Get-ItemProperty $key
        $out += "$svc : Start=$($p.Start) Group=$($p.Group) ImagePath=$($p.ImagePath)"
    } else {
        $out += "$svc : NOT FOUND"
    }
}

[GC]::Collect()
[GC]::WaitForPendingFinalizers()
& reg unload HKLM\OFFLINE_SYS
Dismount-DiskImage -ImagePath 'G:\csmw10.VHD'

$out | Out-File -FilePath "C:\Users\Admin\Desktop\cobdiskless\storage_services_check.txt" -Encoding utf8
