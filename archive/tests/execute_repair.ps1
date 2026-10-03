$ErrorActionPreference = "Continue"
$logFile = "C:\Users\Admin\Desktop\cobdiskless\bootsect_repair.log"

Start-Transcript -Path $logFile -Force

Write-Host "=========================================="
Write-Host "STAGE: REPAIR BOOT CODE (NON-DESTRUCTIVE)"
Write-Host "=========================================="

$vhdPath = "G:\w11.VHD"

# Verify VHD is mounted
$img = Get-DiskImage -ImagePath $vhdPath -ErrorAction SilentlyContinue
if (-not $img -or -not $img.Attached) {
    Write-Host "VHD is not attached. Attaching..."
    $img = Mount-DiskImage -ImagePath $vhdPath -PassThru
    Start-Sleep -Seconds 2
}

$disk = $img | Get-Disk
Write-Host "Attached Disk: $($disk.Number)"
$parts = $disk | Get-Partition
$parts | Format-Table DiskNumber, PartitionNumber, DriveLetter, Size, Type, IsBoot, IsActive

$bootPart = $parts | Where-Object { $_.IsActive -eq $true }
if (-not $bootPart) {
    Write-Host "No active partition found; falling back to Partition 1..."
    $bootPart = $parts | Where-Object { $_.PartitionNumber -eq 1 }
}

$bootLetter = "$($bootPart.DriveLetter):"
Write-Host ("Target Boot Partition: " + $bootLetter)

$winPart = $parts | Where-Object { $_.PartitionNumber -eq 2 }
$winLetter = if ($winPart.DriveLetter) { "$($winPart.DriveLetter):" } else { $null }
Write-Host ("Windows OS Partition : " + $winLetter)

Write-Host ("`nRunning bootsect.exe on " + $bootLetter + " with /mbr /force...")
& bootsect.exe /nt60 $bootLetter /mbr /force
Write-Host ("bootsect exit code for boot partition: " + $LASTEXITCODE)

if ($winLetter) {
    Write-Host ("`nRunning bootsect.exe on " + $winLetter + " with /force...")
    & bootsect.exe /nt60 $winLetter /force
    Write-Host ("bootsect exit code for win partition: " + $LASTEXITCODE)
}

Write-Host "`nVerifying boot files..."
if (Test-Path "$($bootLetter)\bootmgr") {
    $bm = Get-Item "$($bootLetter)\bootmgr"
    Write-Host ("  -> $($bootLetter)\bootmgr present (Size: " + $bm.Length + " bytes)")
}
if (Test-Path "$($bootLetter)\Boot\BCD") {
    $bcd = Get-Item "$($bootLetter)\Boot\BCD"
    Write-Host ("  -> $($bootLetter)\Boot\BCD present (Size: " + $bcd.Length + " bytes)")
}

Write-Host "`nCleanly dismounting VHD $vhdPath..."
Dismount-DiskImage -ImagePath $vhdPath
Start-Sleep -Seconds 2

$check = Get-DiskImage -ImagePath $vhdPath -ErrorAction SilentlyContinue
if ($check -and $check.Attached) {
    Write-Warning "VHD is still attached! Retrying dismount..."
    Dismount-DiskImage -ImagePath $vhdPath
    Start-Sleep -Seconds 2
}

$finalCheck = Get-DiskImage -ImagePath $vhdPath -ErrorAction SilentlyContinue
$status = if ($finalCheck) { $finalCheck.Attached } else { $false }
Write-Host ("Final Attachment Status: " + $status)

Stop-Transcript
