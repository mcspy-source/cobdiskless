$out = "C:\Users\Admin\Desktop\cobdiskless\find_iscsi_inf.txt"
"Searching WinSxS for iscsi.inf..." | Out-File $out -Encoding utf8
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
    Get-ChildItem -Path "$winDrive\Windows\WinSxS" -Filter "*iscsi.inf*" | ForEach-Object {
        "Found folder: $($_.FullName)" | Out-File $out -Append -Encoding utf8
        Get-ChildItem -Path $_.FullName | ForEach-Object {
            "  -> $($_.Name) ($($_.Length) bytes) ver: $((Get-Item $_.FullName).VersionInfo.FileVersion)" | Out-File $out -Append -Encoding utf8
        }
    }
}
Dismount-DiskImage -ImagePath "G:\csmw10.VHD"
"Done." | Out-File $out -Append -Encoding utf8
