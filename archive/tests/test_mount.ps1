$m = Mount-DiskImage -ImagePath 'G:\csmw10.VHD' -PassThru
$disk = $m | Get-Disk
$parts = $disk | Get-Partition
$part2 = $parts | Where-Object { $_.PartitionNumber -eq 2 }
if (-not $part2.DriveLetter) {
    Set-Partition -DiskNumber $disk.Number -PartitionNumber 2 -NewDriveLetter I
}
$sysPath = "I:\Windows\System32\config\SYSTEM"
$res = @()
$res += "sysPath: $sysPath, exists: $(Test-Path $sysPath)"
$res += (& reg load HKLM\OFFLINE_SYS "$sysPath" 2>&1)
$res += (& reg query "HKLM\OFFLINE_SYS\ControlSet001\Control" /v "SystemBootDevice" 2>&1)
& reg unload HKLM\OFFLINE_SYS
Dismount-DiskImage -ImagePath 'G:\csmw10.VHD'
$res | Out-File -FilePath "C:\Users\Admin\Desktop\cobdiskless\sys_root_check.txt" -Encoding utf8
