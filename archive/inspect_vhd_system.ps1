# inspect_vhd_system.ps1
param(
    [string]$VhdPath = "G:\G\csmw10.VHD"
)

$ErrorActionPreference = "Continue"

Write-Host "Mounting VHD..."
$m = Mount-DiskImage -ImagePath $VhdPath -StorageType VHD -PassThru
Start-Sleep -Seconds 2

$disk = Get-DiskImage -ImagePath $VhdPath | Get-Disk
$parts = Get-Partition -DiskNumber $disk.Number
$osPart = $parts | Where-Object { $_.PartitionNumber -eq 2 }

$driveLetter = $osPart.DriveLetter
if (-not $driveLetter) {
    Set-Partition -DiskNumber $disk.Number -PartitionNumber 2 -NewDriveLetter Z
    $driveLetter = 'Z'
}

$drv = "$($driveLetter):"
Write-Host "OS Partition is: $drv"

Write-Host "Files under \Windows\System32\config\SYSTEM*:"
Get-ChildItem -Path "$drv\Windows\System32\config\SYSTEM*" | Format-Table Name, Length, LastWriteTime

Write-Host "Querying live registry from mounted partition:"
& reg.exe load HKLM\VHD_SYS "$drv\Windows\System32\config\SYSTEM"
if ($LASTEXITCODE -eq 0) {
    Write-Host "--- Querying rt640x64 ---"
    & reg.exe query HKLM\VHD_SYS\ControlSet001\Services\rt640x64
    Write-Host "--- Querying msiscsi ---"
    & reg.exe query HKLM\VHD_SYS\ControlSet001\Services\msiscsi
    Write-Host "--- Querying ms_wfplwf ---"
    & reg.exe query HKLM\VHD_SYS\ControlSet001\Services\ms_wfplwf
    Write-Host "--- Querying PagingFiles ---"
    & reg.exe query "HKLM\VHD_SYS\ControlSet001\Control\Session Manager\Memory Management" /v PagingFiles
    Write-Host "--- Querying MountedDevices ---"
    & reg.exe query HKLM\VHD_SYS\MountedDevices

    [gc]::Collect()
    [gc]::WaitForPendingFinalizers()
    Start-Sleep -Milliseconds 500
    & reg.exe unload HKLM\VHD_SYS
} else {
    Write-Host "Failed to load hive into HKLM\VHD_SYS"
}

Dismount-DiskImage -ImagePath $VhdPath
Write-Host "Dismounted."
