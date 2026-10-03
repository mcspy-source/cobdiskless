$m = Mount-DiskImage -ImagePath 'G:\csmw10.VHD' -PassThru
$disk = $m | Get-Disk
$parts = $disk | Get-Partition
$out = @()
foreach ($p in $parts) {
    $out += "Part $($p.PartitionNumber): DriveLetter=$($p.DriveLetter), Size=$($p.Size)"
    if ($p.PartitionNumber -eq 2 -and -not $p.DriveLetter) {
        Set-Partition -DiskNumber $disk.Number -PartitionNumber 2 -NewDriveLetter Z
        $out += "Assigned Z: to partition 2"
    }
}

$vol2 = Get-Partition -DiskNumber $disk.Number -PartitionNumber 2
$dl = $vol2.DriveLetter
$drvPath = "${dl}:\Windows\System32\drivers\rt640x64.sys"
if (Test-Path $drvPath) {
    $ver = (Get-Item $drvPath).VersionInfo
    $out += "Driver rt640x64.sys: FileVersion=$($ver.FileVersion), ProductVersion=$($ver.ProductVersion), Size=$((Get-Item $drvPath).Length)"
} else {
    $out += "Driver rt640x64.sys not found at $drvPath"
}

Dismount-DiskImage -ImagePath 'G:\csmw10.VHD'
$out | Out-File -FilePath "C:\Users\Admin\Desktop\cobdiskless\vhd_part_and_driver.txt" -Encoding utf8
