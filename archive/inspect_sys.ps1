$vhd = "G:\G\csmw10.VHD"
$di = Mount-DiskImage -ImagePath $vhd -PassThru
$disk = $di | Get-Disk
$parts = $disk | Get-Partition
$vol = $parts | Where-Object { $_.DriveLetter } | Select-Object -First 1
Write-Host "Partition DriveLetter: $($vol.DriveLetter)"
if ($vol.DriveLetter) {
    $drive = "$($vol.DriveLetter):"
    $sysPath = "$drive\Windows\System32\config\SYSTEM"
    Write-Host "Loading hive from $sysPath"
    & reg.exe load HKLM\CHECK_SYS $sysPath
    Write-Host "=== Select ==="
    & reg.exe query HKLM\CHECK_SYS\Select
    Write-Host "=== Memory Management ==="
    & reg.exe query "HKLM\CHECK_SYS\ControlSet001\Control\Session Manager\Memory Management" /v PagingFiles
    Write-Host "=== MountedDevices ==="
    & reg.exe query HKLM\CHECK_SYS\MountedDevices
    Write-Host "=== ms_wfplwf ==="
    & reg.exe query HKLM\CHECK_SYS\ControlSet001\Services\ms_wfplwf
    Write-Host "=== rt640x64 ==="
    & reg.exe query HKLM\CHECK_SYS\ControlSet001\Services\rt640x64
    Write-Host "Unloading hive..."
    & reg.exe unload HKLM\CHECK_SYS
}
Dismount-DiskImage -ImagePath $vhd
Write-Host "Done"
