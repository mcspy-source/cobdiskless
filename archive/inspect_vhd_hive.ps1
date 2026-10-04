$img = "G:\G\csmw10.VHD"
$m = Mount-DiskImage -ImagePath $img -PassThru
$disk = $m | Get-Disk
"Disk number: " + $disk.Number
$parts = $disk | Get-Partition
$parts | Format-Table DiskNumber, PartitionNumber, DriveLetter, Size

foreach ($p in $parts) {
    if (-not $p.DriveLetter) {
        $avail = [char[]]([char]'H'..[char]'Z') | Where-Object { (Get-Volume).DriveLetter -notcontains $_ }
        Set-Partition -DiskNumber $p.DiskNumber -PartitionNumber $p.PartitionNumber -NewDriveLetter $avail[0] -ErrorAction SilentlyContinue
    }
}

Get-Volume | Format-Table DriveLetter, FileSystemLabel, Size

# List config files
foreach ($d in ([char[]]([char]'H'..[char]'Z'))) {
    $cfg = "$($d):\Windows\System32\config"
    if (Test-Path $cfg) {
        "Found Windows config on drive $d"
        Get-ChildItem -Path $cfg | Format-Table Name, Length, LastWriteTime
    }
}

Dismount-DiskImage -ImagePath $img
