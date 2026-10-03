$out = "C:\Users\Admin\Desktop\cobdiskless\find_in_vhd_sx.txt"
"Searching recently modified in WinSxS..." | Out-File $out -Encoding utf8
$m = Mount-DiskImage -ImagePath "G:\csmw10.VHD" -PassThru
$letters = ($m | Get-Disk | Get-Partition | Get-Volume).DriveLetter | Where-Object { $_ }
$winDrive = $null
foreach ($l in $letters) {
    if (Test-Path "${l}:\Windows\System32") {
        $winDrive = "${l}:\"
        break
    }
}
"Windows drive: $winDrive" | Out-File $out -Append -Encoding utf8

if ($winDrive) {
    $now = Get-Date
    Get-ChildItem -Path "$winDrive\Windows\WinSxS" -Recurse | Where-Object { ($now - $_.LastWriteTime).TotalMinutes -lt 15 } | ForEach-Object {
        "Recent: $($_.FullName)" | Out-File $out -Append -Encoding utf8
    }
}
Dismount-DiskImage -ImagePath "G:\csmw10.VHD"
"Done." | Out-File $out -Append -Encoding utf8
