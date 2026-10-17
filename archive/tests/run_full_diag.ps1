$m = Mount-DiskImage -ImagePath 'G:\csmw10.VHD' -PassThru
$disk = $m | Get-Disk
$parts = $disk | Get-Partition
$part2 = $parts | Where-Object { $_.PartitionNumber -eq 2 }
$vhdVol = Get-Volume | Where-Object { $_.UniqueId -match $disk.Number }
if (-not $part2.DriveLetter) {
    Set-Partition -DiskNumber $disk.Number -PartitionNumber 2 -NewDriveLetter Z
    $vhdLetter = 'Z'
} else {
    $vhdLetter = $part2.DriveLetter
}
$sysPath = "${vhdLetter}:\Windows\System32\config\SYSTEM"

$res = @()
$res += "sysPath: $sysPath, exists: $(Test-Path $sysPath)"
& reg load HKLM\OFFLINE_SYS "$sysPath"

$res += "=== ControlSet001 Services rt640x64 ==="
$res += (& reg query "HKLM\OFFLINE_SYS\ControlSet001\Services\rt640x64" /s 2>&1)

$res += "=== ControlSet001 Services iScsiPrt ==="
$res += (& reg query "HKLM\OFFLINE_SYS\ControlSet001\Services\iScsiPrt" /s 2>&1)

$res += "=== ControlSet001 Control ServiceGroupOrder ==="
$res += (& reg query "HKLM\OFFLINE_SYS\ControlSet001\Control\ServiceGroupOrder" /v "List" 2>&1)

$res += "=== ControlSet001 Control CriticalDeviceDatabase ==="
$res += (& reg query "HKLM\OFFLINE_SYS\ControlSet001\Control\CriticalDeviceDatabase" /s 2>&1)

$res += "=== MountedDevices ==="
$res += (& reg query "HKLM\OFFLINE_SYS\MountedDevices" 2>&1)

$res += "=== Tcpip Parameters Interfaces ==="
$res += (& reg query "HKLM\OFFLINE_SYS\ControlSet001\Services\Tcpip\Parameters\Interfaces" /s 2>&1)

& reg unload HKLM\OFFLINE_SYS
Remove-PartitionAccessPath -DiskNumber $disk.Number -PartitionNumber 2 -Accesspath "${vhdLetter}:" -ErrorAction SilentlyContinue
Dismount-DiskImage -ImagePath 'G:\csmw10.VHD'

$res | Out-File -FilePath "C:\Users\Admin\Desktop\cobdiskless\vhd_actual_diag.txt" -Encoding utf8
