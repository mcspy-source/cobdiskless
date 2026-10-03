$m = Mount-DiskImage -ImagePath 'G:\csmw10.VHD' -PassThru
$out = @()
$out += "Mount object: Attached=$($m.Attached), Number=$($m.Number)"
$disks = Get-Disk
foreach ($d in $disks) {
    $out += "Disk $($d.Number): FriendlyName=$($d.FriendlyName), Path=$($d.Path), Size=$($d.Size)"
    $partitions = Get-Partition -DiskNumber $d.Number -ErrorAction SilentlyContinue
    foreach ($p in $partitions) {
        $out += "  Part $($p.PartitionNumber): DriveLetter=$($p.DriveLetter), Size=$($p.Size)"
    }
}
Dismount-DiskImage -ImagePath 'G:\csmw10.VHD'
$out | Out-File -FilePath "C:\Users\Admin\Desktop\cobdiskless\vhd_disks_dump.txt" -Encoding utf8
