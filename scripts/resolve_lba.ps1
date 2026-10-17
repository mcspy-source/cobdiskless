# CobDiskless - PowerShell LBA to File Resolution Script
# Requires Elevated / Administrative Privileges

param(
    [string]$VhdPath = ".\images\csmw10.VHD",
    [int64]$TargetLba = 0x0012aaf0
)

Write-Host "=========================================================" -ForegroundColor Cyan
Write-Host "CobDiskless LBA to File Resolver (PowerShell / fsutil)" -ForegroundColor Cyan
Write-Host "=========================================================" -ForegroundColor Cyan
Write-Host "VHD Path:   $VhdPath"
Write-Host "Target LBA: 0x$($TargetLba.ToString('X8')) ($TargetLba sectors)"

if (-not (Test-Path $VhdPath)) {
    Write-Error "VHD file not found at $VhdPath"
    exit 1
}

$mounted = $false
try {
    Write-Host "[1] Mounting VHD as Read-Only..." -ForegroundColor Yellow
    $diskImg = Mount-DiskImage -ImagePath $VhdPath -Access ReadOnly -PassThru
    $mounted = $true

    $disk = Get-DiskImage -ImagePath $VhdPath | Get-Disk
    Write-Host "    Mounted Disk Number: $($disk.Number)"

    Write-Host "[2] Querying Partitions..." -ForegroundColor Yellow
    $partitions = Get-Partition -DiskNumber $disk.Number | Where-Object { $_.Type -ne "Reserved" -and $_.DriveLetter }
    
    $targetPartition = $null
    foreach ($part in (Get-Partition -DiskNumber $disk.Number)) {
        $startLba = [int64]($part.Offset / 512)
        $endLba = $startLba + [int64]($part.Size / 512)
        Write-Host "    Partition $($part.PartitionNumber): Start LBA=$startLba, End LBA=$endLba, DriveLetter=$($part.DriveLetter):"
        if ($TargetLba -ge $startLba -and $TargetLba -lt $endLba) {
            $targetPartition = $part
            break
        }
    }

    if (-not $targetPartition) {
        Write-Error "Target LBA $TargetLba does not fall inside any partition!"
        exit 1
    }

    $partStartLba = [int64]($targetPartition.Offset / 512)
    $relativeLba = $TargetLba - $partStartLba
    $driveLetter = "$($targetPartition.DriveLetter):"

    Write-Host "[3] Calculating Relative Sector & Cluster..." -ForegroundColor Yellow
    Write-Host "    Partition Start LBA: $partStartLba"
    Write-Host "    Relative Sector:     $relativeLba"

    # Query NTFS cluster size
    $vol = Get-Volume -DriveLetter $targetPartition.DriveLetter
    $sectorsPerCluster = ($vol.AllocationUnitSize / 512)
    if (-not $sectorsPerCluster -or $sectorsPerCluster -eq 0) { $sectorsPerCluster = 8 }
    $cluster = [math]::Floor($relativeLba / $sectorsPerCluster)

    Write-Host "    Sectors Per Cluster: $sectorsPerCluster"
    Write-Host "    Target Cluster (LCN): $cluster"

    Write-Host "[4] Querying NTFS File ID via fsutil..." -ForegroundColor Yellow
    $clusterOutput = fsutil file querycluster $driveLetter $cluster
    Write-Host "    fsutil querycluster output:`n$clusterOutput"

    # Extract File ID / File Reference Number
    # Example format: "Cluster 12126 used by ... File reference number is 0x00000000000032f2"
    if ($clusterOutput -match "0x([0-9a-fA-F]+)") {
        $fileId = $matches[0]
        Write-Host "[5] Querying File Path via File ID: $fileId..." -ForegroundColor Green
        $nameOutput = fsutil file queryfilenamebyid $driveLetter\ $fileId
        Write-Host "    fsutil queryfilenamebyid output:`n$nameOutput"
    } else {
        Write-Warning "Could not extract File ID regex from fsutil output."
    }

} catch {
    Write-Error "Exception occurred: $_"
} finally {
    if ($mounted) {
        Write-Host "[6] Dismounting VHD..." -ForegroundColor Yellow
        Dismount-DiskImage -ImagePath $VhdPath -ErrorAction SilentlyContinue | Out-Null
        Write-Host "    VHD dismounted cleanly." -ForegroundColor Green
    }
}
