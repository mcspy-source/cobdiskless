$out = "C:\Users\Admin\Desktop\cobdiskless\w11_check_out.txt"
"Checking w11eufi.VHD..." | Out-File $out -Encoding utf8
$m = Mount-DiskImage -ImagePath "G:\w11eufi.VHD" -PassThru
$letters = ($m | Get-Disk | Get-Partition | Get-Volume).DriveLetter | Where-Object { $_ }
$winDrive = $null
foreach ($l in $letters) {
    if (Test-Path "${l}:\Windows\System32") {
        $winDrive = "${l}:"
        break
    }
}
"Windows drive: $winDrive" | Out-File $out -Append -Encoding utf8

if ($winDrive) {
    $nt = "$winDrive\Windows\System32\ntoskrnl.exe"
    "ntoskrnl.exe ver: $((Get-Item $nt).VersionInfo.FileVersion)" | Out-File $out -Append -Encoding utf8
    $ms = "$winDrive\Windows\System32\drivers\msiscsi.sys"
    "msiscsi.sys ver: $((Get-Item $ms).VersionInfo.FileVersion)" | Out-File $out -Append -Encoding utf8
}
Dismount-DiskImage -ImagePath "G:\w11eufi.VHD"
"Done." | Out-File $out -Append -Encoding utf8
