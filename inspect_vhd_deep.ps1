$target = 'G:\G\csmw10.vhd'
$vhd = Mount-VHD -Path $target -Passthru -NoDriveLetter
$part = Get-Partition -DiskNumber $vhd.DiskNumber | Where-Object { $_.Size -gt 1GB } | Select-Object -First 1
$access = Add-PartitionAccessPath -DiskNumber $part.DiskNumber -PartitionNumber $part.PartitionNumber -AssignDriveLetter -PassThru
$drive = $access.AccessPaths | Where-Object { $_ -match '^[A-Z]:\\$' }
$sys = Join-Path $drive 'Windows\System32\config\SYSTEM'
reg.exe load HKLM\CHECK_SYS $sys

Get-ItemProperty 'HKLM:\CHECK_SYS\ControlSet001\Control\Class\{4d36e972-e325-11ce-bfc1-08002be10318}\0000' | Format-List * | Out-File -FilePath 'C:\Users\Admin\Desktop\cobecosystem\cobdiskless\vhd_0000.txt' -Encoding ascii

Get-ItemProperty 'HKLM:\CHECK_SYS\ControlSet001\Services\Rt640x64' | Format-List * | Out-File -FilePath 'C:\Users\Admin\Desktop\cobecosystem\cobdiskless\vhd_rt640x64.txt' -Encoding ascii

Get-ChildItem 'HKLM:\CHECK_SYS\ControlSet001\Control\NetworkSetup2\Interfaces\{820C4568-D585-420E-94B3-AEA0E0DC3402}\Properties\*' | ForEach-Object {
    $parent = $_.PSChildName
    Get-ChildItem $_.PSPath | ForEach-Object {
        $val = Get-ItemProperty $_.PSPath
        [PSCustomObject]@{
            Guid = $parent
            Pid = $_.PSChildName
            Value = $val.'(default)'
        }
    }
} | Format-Table -AutoSize | Out-File -FilePath 'C:\Users\Admin\Desktop\cobecosystem\cobdiskless\vhd_ns2.txt' -Encoding ascii

reg.exe unload HKLM\CHECK_SYS
Dismount-VHD -Path $target
