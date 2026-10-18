$target = 'G:\G\csmw10.vhd'
$vhd = Mount-VHD -Path $target -Passthru -NoDriveLetter
$part = Get-Partition -DiskNumber $vhd.DiskNumber | Where-Object { $_.Size -gt 1GB } | Select-Object -First 1
$access = Add-PartitionAccessPath -DiskNumber $part.DiskNumber -PartitionNumber $part.PartitionNumber -AssignDriveLetter -PassThru
$drive = $access.AccessPaths | Where-Object { $_ -match '^[A-Z]:\\$' }
$sys = Join-Path $drive 'Windows\System32\config\SYSTEM'
reg.exe load HKLM\CHECK_SYS $sys

Get-ChildItem 'HKLM:\CHECK_SYS\ControlSet001\Control\Class\{4d36e972-e325-11ce-bfc1-08002be10318}' | ForEach-Object {
    $props = Get-ItemProperty $_.PSPath
    [PSCustomObject]@{
        Name = $_.PSChildName
        DriverDesc = $props.DriverDesc
        MatchingDeviceId = $props.MatchingDeviceId
        NetCfgInstanceId = $props.NetCfgInstanceId
        DeviceInstanceID = $props.DeviceInstanceID
    }
} | Format-Table -AutoSize | Out-File -FilePath 'C:\Users\Admin\Desktop\cobecosystem\cobdiskless\vhd_adapters.txt' -Encoding ascii

reg.exe unload HKLM\CHECK_SYS
Dismount-VHD -Path $target
