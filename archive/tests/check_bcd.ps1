$out = "C:\Users\Admin\Desktop\cobdiskless\bcd_out.txt"
$m = Mount-DiskImage -ImagePath "G:\csmw10.VHD" -PassThru
$letters = ($m | Get-Disk | Get-Partition | Get-Volume).DriveLetter | Where-Object { $_ }
$bootDrive = $null
foreach ($l in $letters) {
    if (Test-Path "${l}:\Boot\BCD") {
        $bootDrive = "${l}:"
        break
    }
}
"Boot drive with BCD: $bootDrive" | Out-File $out -Encoding utf8

if ($bootDrive) {
    $bcd = "$bootDrive\Boot\BCD"
    & bcdedit.exe /store $bcd /enum all 2>&1 | Out-File $out -Append -Encoding utf8
}
Dismount-DiskImage -ImagePath "G:\csmw10.VHD"
