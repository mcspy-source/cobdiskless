$m = Mount-DiskImage -ImagePath 'G:\csmw10.VHD' -PassThru
$disk = Get-DiskImage -ImagePath 'G:\csmw10.VHD' | Get-Disk
$parts = $disk | Get-Partition
foreach ($p in $parts) {
    $vol = $p | Get-Volume -ErrorAction SilentlyContinue
    Write-Host "Part $($p.PartitionNumber): DriveLetter=$($p.DriveLetter) Size=$($p.Size) Vol=$($vol.FileSystemLabel)"
}
Dismount-DiskImage -ImagePath 'G:\csmw10.VHD'
