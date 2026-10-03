$m = Mount-DiskImage -ImagePath 'G:\csmw10.VHD' -PassThru
$parts = $m | Get-Disk | Get-Partition
$osPart = $parts | Where-Object { Test-Path "$($_.DriveLetter):\Windows\System32\config\SYSTEM" }
& reg load HKLM\OFFLINE_SYS "$($osPart.DriveLetter):\Windows\System32\config\SYSTEM"
Start-Sleep -Milliseconds 500

$mdKey = "HKLM:\OFFLINE_SYS\MountedDevices"
$props = (Get-Item $mdKey).Property
$out = @()
foreach ($p in $props) {
    $val = (Get-ItemProperty $mdKey).$p
    $hex = ($val | ForEach-Object { '{0:X2}' -f $_ }) -join ' '
    $out += "$p = $hex"
}

[GC]::Collect()
[GC]::WaitForPendingFinalizers()
& reg unload HKLM\OFFLINE_SYS
Dismount-DiskImage -ImagePath 'G:\csmw10.VHD'
$out | Out-File -FilePath "C:\Users\Admin\Desktop\cobdiskless\md_verify.txt" -Encoding utf8
