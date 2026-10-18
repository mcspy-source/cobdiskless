$target = 'G:\G\csmw10.vhd'
$vhd = Mount-VHD -Path $target -Passthru -NoDriveLetter
$part = Get-Partition -DiskNumber $vhd.DiskNumber | Where-Object { $_.Size -gt 1GB } | Select-Object -First 1
$access = Add-PartitionAccessPath -DiskNumber $part.DiskNumber -PartitionNumber $part.PartitionNumber -AssignDriveLetter -PassThru
$drive = $access.AccessPaths | Where-Object { $_ -match '^[A-Z]:\\$' }
$sys = Join-Path $drive 'Windows\System32\config\SYSTEM'
reg.exe load HKLM\CHECK_SYS $sys

$path = 'HKLM:\CHECK_SYS\ControlSet001\Enum\PCI\VEN_10EC&DEV_8168&SUBSYS_307C17AA&REV_06\3&11583659&0&18'
if (Test-Path $path) {
    Write-Output "KEY EXISTS!"
    Get-ItemProperty $path | Format-List *
} else {
    Write-Output "KEY DOES NOT EXIST!"
    Get-ChildItem 'HKLM:\CHECK_SYS\ControlSet001\Enum\PCI' | Where-Object { $_.PSChildName -like '*307C17AA*' } | ForEach-Object {
        Write-Output ("Found: " + $_.PSChildName)
    }
}

reg.exe unload HKLM\CHECK_SYS
Dismount-VHD -Path $target
