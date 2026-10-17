$m = Mount-DiskImage -ImagePath 'G:\csmw10.VHD' -PassThru
$disk = $m | Get-Disk
$parts = $disk | Get-Partition
$res = @()
foreach ($p in $parts) {
    $res += "Partition $($p.PartitionNumber): Type=$($p.Type) Size=$($p.Size) DriveLetter=$($p.DriveLetter)"
    if (-not $p.DriveLetter) {
        $letter = [char](65 + $p.PartitionNumber + 10) # K, L, etc
        Set-Partition -DiskNumber $disk.Number -PartitionNumber $p.PartitionNumber -NewDriveLetter $letter -ErrorAction SilentlyContinue
        $p = Get-Partition -DiskNumber $disk.Number -PartitionNumber $p.PartitionNumber
    }
    if ($p.DriveLetter) {
        $files = Get-ChildItem -Path "$($p.DriveLetter):\" -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Name
        $res += "  Files on $($p.DriveLetter): " + ($files -join ", ")
        $sys = "$($p.DriveLetter):\Windows\System32\config\SYSTEM"
        $res += "  SYSTEM exists: $(Test-Path $sys)"
    }
}
Dismount-DiskImage -ImagePath 'G:\csmw10.VHD'
$res | Out-File -FilePath "C:\Users\Admin\Desktop\cobdiskless\vhd_part_search.txt" -Encoding utf8
