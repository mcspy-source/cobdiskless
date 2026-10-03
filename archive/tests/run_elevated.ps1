$ErrorActionPreference = "Continue"
$logFile = "C:\Users\Admin\Desktop\cobdiskless\elevated_run.log"

Start-Transcript -Path $logFile -Force

Write-Host "Mounting G:\w11.VHD..."
$m = Mount-DiskImage -ImagePath "G:\w11.VHD" -PassThru
Start-Sleep -Seconds 2

$disk = $m | Get-Disk
Write-Host "Mounted Disk Number: $($disk.Number)"
Write-Host "Partition Style: $($disk.PartitionStyle)"

$partitions = $disk | Get-Partition
$partitions | Format-Table DiskNumber, PartitionNumber, DriveLetter, Size, Type, IsBoot, IsActive

$volumes = $partitions | Get-Volume
$volumes | Format-Table DriveLetter, FileSystemLabel, FileSystem, SizeRemaining, Size

# Let's inspect each partition's drive letter and files
foreach ($p in $partitions) {
    Write-Host "Partition $($p.PartitionNumber): DriveLetter=$($p.DriveLetter), Offset=$($p.Offset), Size=$($p.Size)"
}

Write-Host "Checking for boot files..."
foreach ($p in $partitions) {
    if ($p.DriveLetter) {
        $dl = "$($p.DriveLetter):"
        Write-Host "Drive $dl contents:"
        Get-ChildItem -Path "$dl\" -Force -ErrorAction SilentlyContinue | Select-Object Name, Length, Mode | Format-Table | Out-String | Write-Host
        
        if (Test-Path "$dl\bootmgr") {
            Write-Host "FOUND $dl\bootmgr !"
        }
        if (Test-Path "$dl\Boot") {
            Write-Host "FOUND $dl\Boot directory!"
            Get-ChildItem -Path "$dl\Boot" -Force -ErrorAction SilentlyContinue | Select-Object Name | Format-Table | Out-String | Write-Host
        }
        if (Test-Path "$dl\EFI") {
            Write-Host "FOUND $dl\EFI directory!"
            Get-ChildItem -Path "$dl\EFI" -Recurse -Force -ErrorAction SilentlyContinue | Select-Object FullName | Format-Table | Out-String | Write-Host
        }
        if (Test-Path "$dl\Windows\Boot\EFI\bootmgfw.efi") {
            Write-Host "FOUND $dl\Windows\Boot\EFI\bootmgfw.efi !"
        }
    }
}

Stop-Transcript
