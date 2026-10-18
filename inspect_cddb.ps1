$target = 'G:\G\csmw10.vhd'
$vhd = Mount-VHD -Path $target -Passthru -NoDriveLetter
$part = Get-Partition -DiskNumber $vhd.DiskNumber | Where-Object { $_.Size -gt 1GB } | Select-Object -First 1
$access = Add-PartitionAccessPath -DiskNumber $part.DiskNumber -PartitionNumber $part.PartitionNumber -AssignDriveLetter -PassThru
$drive = $access.AccessPaths | Where-Object { $_ -match '^[A-Z]:\\$' }
$sys = Join-Path $drive 'Windows\System32\config\SYSTEM'
reg.exe load HKLM\CHECK_SYS $sys

Get-ChildItem 'HKLM:\CHECK_SYS\ControlSet001\Control\CriticalDeviceDatabase' | Where-Object { $_.PSChildName -like '*10ec*' } | ForEach-Object {
    $p = Get-ItemProperty $_.PSPath
    [PSCustomObject]@{
        Key = $_.PSChildName
        Service = $p.Service
        ClassGUID = $p.ClassGUID
    }
} | Format-Table -AutoSize | Out-File -FilePath 'C:\Users\Admin\Desktop\cobecosystem\cobdiskless\vhd_cddb_10ec.txt' -Encoding ascii

reg.exe unload HKLM\CHECK_SYS
Dismount-VHD -Path $target
