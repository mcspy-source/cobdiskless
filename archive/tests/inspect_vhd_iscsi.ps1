$m = Mount-DiskImage -ImagePath 'G:\csmw10.VHD' -PassThru
$parts = $m | Get-Disk | Get-Partition
$osPart = $parts | Where-Object { Test-Path "$($_.DriveLetter):\Windows\System32\config\SYSTEM" }
$dl = $osPart.DriveLetter
Write-Host "OS Drive Letter: $dl"
Write-Host "msiscsi.sys exists: $(Test-Path "$dl`:\Windows\System32\drivers\msiscsi.sys")"
Write-Host "iscsiprt.sys exists: $(Test-Path "$dl`:\Windows\System32\drivers\iscsiprt.sys")"
Write-Host "iscsi.inf exists: $(Test-Path "$dl`:\Windows\INF\iscsi.inf")"

& reg load HKLM\OFFLINE_SYS "$dl`:\Windows\System32\config\SYSTEM"
Write-Host "--- Select ---"
& reg query HKLM\OFFLINE_SYS\Select 2>&1

Write-Host "--- iScsiPrt in ControlSet001 ---"
& reg query HKLM\OFFLINE_SYS\ControlSet001\Services\iScsiPrt /s 2>&1

Write-Host "--- msiscsi in ControlSet001 ---"
& reg query HKLM\OFFLINE_SYS\ControlSet001\Services\msiscsi /s 2>&1

Write-Host "--- SCSI Class ---"
& reg query "HKLM\OFFLINE_SYS\ControlSet001\Control\Class\{4d36e97b-e325-11ce-bfc1-08002be10318}" /s 2>&1

[GC]::Collect()
[GC]::WaitForPendingFinalizers()
& reg unload HKLM\OFFLINE_SYS
Dismount-DiskImage -ImagePath 'G:\csmw10.VHD'
