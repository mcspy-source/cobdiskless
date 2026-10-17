$target = 'G:\G\csmw10.vhd'
$vhd = Mount-VHD -Path $target -Passthru -NoDriveLetter
$part = Get-Partition -DiskNumber $vhd.DiskNumber | Where-Object { $_.Size -gt 1GB } | Select-Object -First 1
$access = Add-PartitionAccessPath -DiskNumber $part.DiskNumber -PartitionNumber $part.PartitionNumber -AssignDriveLetter -PassThru
$drive = $access.AccessPaths | Where-Object { $_ -match '^[A-Z]:\\$' }
$sys = Join-Path $drive 'Windows\System32\config\SYSTEM'
reg.exe load HKLM\CHECK_SYS $sys

Get-ChildItem 'HKLM:\CHECK_SYS\ControlSet001\Enum\PCI\*' | Where-Object { $_.PSChildName -like '*8168*' } | ForEach-Object {
    $parent = $_.PSChildName
    Get-ChildItem $_.PSPath | ForEach-Object {
        $p = Get-ItemProperty $_.PSPath
        [PSCustomObject]@{
            Dev = $parent
            Inst = $_.PSChildName
            Driver = $p.Driver
            Service = $p.Service
        }
    }
} | Format-Table -AutoSize | Out-File -FilePath 'C:\Users\Admin\Desktop\cobecosystem\cobdiskless\vhd_enum_8168.txt' -Encoding ascii

reg.exe unload HKLM\CHECK_SYS
Dismount-VHD -Path $target
