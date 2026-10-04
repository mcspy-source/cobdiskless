# dump_hw_keys.ps1
$vhd = "G:\G\csmw10.VHD"
$m = Mount-DiskImage -ImagePath $vhd -StorageType VHD -PassThru
Start-Sleep -Seconds 2
$disk = Get-DiskImage -ImagePath $vhd | Get-Disk
$parts = Get-Partition -DiskNumber $disk.Number
$osPart = $parts | Where-Object { $_.PartitionNumber -eq 2 }
$drv = $osPart.DriveLetter
if (-not $drv) {
    Set-Partition -DiskNumber $disk.Number -PartitionNumber 2 -NewDriveLetter Z
    $drv = 'Z'
}
$osPath = "$($drv):"

& reg.exe load HKLM\VHD_SYS "$osPath\Windows\System32\config\SYSTEM"
& reg.exe query "HKLM\VHD_SYS\ControlSet001\Control\Class\{4d36e972-e325-11ce-bfc1-08002be10318}" /s > C:\Users\Admin\Desktop\cobdiskless\tools\query_net_class.txt 2>&1
& reg.exe query "HKLM\VHD_SYS\ControlSet001\Enum\PCI" /s >> C:\Users\Admin\Desktop\cobdiskless\tools\query_net_class.txt 2>&1
& reg.exe query "HKLM\VHD_SYS\ControlSet001\Control\CriticalDeviceDatabase" /s >> C:\Users\Admin\Desktop\cobdiskless\tools\query_net_class.txt 2>&1

[gc]::Collect()
[gc]::WaitForPendingFinalizers()
Start-Sleep -Milliseconds 500
& reg.exe unload HKLM\VHD_SYS
Dismount-DiskImage -ImagePath $vhd
Write-Host "HW keys dumped successfully."
