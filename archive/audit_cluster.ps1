$vhd = "G:\G\csmw10.VHD"
Write-Host "Mounting VHD: $vhd"
Mount-DiskImage -ImagePath $vhd -StorageType VHD -Access ReadOnly
Start-Sleep -Seconds 2

$img = Get-DiskImage -ImagePath $vhd
$disk = Get-Disk -Number $img.Number
Write-Host "Disk Number: $($disk.Number)"

$partitions = Get-Partition -DiskNumber $disk.Number
$partitions | Format-Table -AutoSize

# Find partition 2
$p2 = $partitions | Where-Object { $_.PartitionNumber -eq 2 }
if (-not $p2.DriveLetter) {
    # Assign drive letter if not present
    $freeLetter = (Get-ChildItem function:[d-z]: -n | Where-Object { -not (Test-Path $_) } | Select-Object -First 1)
    if ($freeLetter) {
        $letter = $freeLetter.Substring(0, 1)
        Set-Partition -DiskNumber $disk.Number -PartitionNumber 2 -NewDriveLetter $letter
        $drive = "$($letter):"
    }
} else {
    $drive = "$($p2.DriveLetter):"
}

Write-Host "Target Partition 2 Drive: $drive"
Write-Host "Partition Start Offset: $($p2.Offset) bytes ($($p2.Offset / 512) LBA)"

# Query cluster
Write-Host "Querying Cluster 279582 on $drive using fsutil volume querycluster..."
$clusterLines = & fsutil volume querycluster $drive 279582
Write-Host "Cluster output:"
$clusterLines | ForEach-Object { Write-Host "  $_" }

# Extract File ID if present
$foundFileId = $null
foreach ($line in $clusterLines) {
    if ($line -match "0x([0-9a-fA-F]{16}|[0-9a-fA-F]{8})") {
        $foundFileId = $matches[0]
        break
    }
}

if ($foundFileId) {
    Write-Host "Found File ID: $foundFileId"
    $fileOut = & fsutil file queryfilenamebyid "$drive\" $foundFileId
    Write-Host "Filename by ID:"
    Write-Host $fileOut
} else {
    Write-Host "Trying fsutil file layout or searching for files mapping to cluster 279582..."
}

# Also let's inspect the raw bytes at LBA 3363056 in the VHD file directly
$vhdStream = [System.IO.File]::Open($vhd, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite)
$offset = [int64]3363056 * [int64]512
$vhdStream.Seek($offset, [System.IO.SeekOrigin]::Begin)
$buf = New-Object byte[] 12800
$read = $vhdStream.Read($buf, 0, 12800)
$vhdStream.Close()

Write-Host "Read $read bytes at raw VHD offset $offset"
# Check PE signature
if ($buf[0] -eq 0x4D -and $buf[1] -eq 0x5A) {
    Write-Host "MZ signature detected!"
    $e_lfanew = [BitConverter]::ToInt32($buf, 0x3C)
    Write-Host "PE offset: $e_lfanew"
    if ($e_lfanew -lt 12000) {
        $peSig = [System.Text.Encoding]::ASCII.GetString($buf, $e_lfanew, 4)
        Write-Host "Signature at PE offset: $peSig"
    }
}

# Scan ASCII strings > 4 chars in buffer
$strings = @()
$curr = New-Object System.Text.StringBuilder
for ($i = 0; $i -lt $read; $i++) {
    $b = $buf[$i]
    if ($b -ge 32 -and $b -le 126) {
        [void]$curr.Append([char]$b)
    } else {
        if ($curr.Length -ge 4) {
            $strings += $curr.ToString()
        }
        $curr.Clear()
    }
}

Write-Host "Sample printable ASCII strings found in LBA 3363056 block:"
$strings | Select-Object -First 30 | ForEach-Object { Write-Host "  $_" }

# Clean up / Dismount
# Note: keep mounted if needed or dismount at end
# Dismount-DiskImage -ImagePath $vhd
