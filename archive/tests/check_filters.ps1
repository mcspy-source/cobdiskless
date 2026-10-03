$m = Mount-DiskImage -ImagePath 'G:\csmw10.VHD' -PassThru
$parts = $m | Get-Disk | Get-Partition
$osPart = $parts | Where-Object { Test-Path "$($_.DriveLetter):\Windows\System32" }
$dl = $osPart.DriveLetter
$out = @()
& reg load HKLM\OFFLINE_SYS "$dl`:\Windows\System32\config\SYSTEM"

$out += "=== Adapter 0000 Linkage ==="
$out += (& reg query "HKLM\OFFLINE_SYS\ControlSet001\Control\Class\{4D36E972-E325-11CE-BFC1-08002BE10318}\0000\Linkage" /s 2>&1)

$out += "=== All Linkage under Net Class ==="
$out += (& reg query "HKLM\OFFLINE_SYS\ControlSet001\Control\Class\{4D36E972-E325-11CE-BFC1-08002BE10318}" /s /f "Linkage" 2>&1)

$out += "=== Services ms_wfplwf ==="
$out += (& reg query "HKLM\OFFLINE_SYS\ControlSet001\Services\ms_wfplwf" /s 2>&1)

$out += "=== Services wfplwfs ==="
$out += (& reg query "HKLM\OFFLINE_SYS\ControlSet001\Services\wfplwfs" /s 2>&1)

$out += "=== Services WfpLwf ==="
$out += (& reg query "HKLM\OFFLINE_SYS\ControlSet001\Services\WfpLwf" /s 2>&1)

[GC]::Collect()
[GC]::WaitForPendingFinalizers()
& reg unload HKLM\OFFLINE_SYS
Dismount-DiskImage -ImagePath 'G:\csmw10.VHD'
$out | Out-File -FilePath "C:\Users\Admin\Desktop\cobdiskless\filter_check.txt" -Encoding utf8
