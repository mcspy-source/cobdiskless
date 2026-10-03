$out = "C:\Users\Admin\Desktop\cobdiskless\vhd_sx_out.txt"
"Starting search in VHD..." | Out-File $out -Encoding utf8
$m = Mount-DiskImage -ImagePath "G:\csmw10.VHD" -PassThru
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
    Get-ChildItem -Path "$winDrive\Windows\WinSxS" -Filter "*iscsi*" -Directory -ErrorAction SilentlyContinue | ForEach-Object {
        "WinSxS dir: $($_.FullName)" | Out-File $out -Append -Encoding utf8
        Get-ChildItem -Path $_.FullName | ForEach-Object {
            "  -> file: $($_.Name) ($($_.Length) bytes) ver: $((Get-Item $_.FullName).VersionInfo.FileVersion)" | Out-File $out -Append -Encoding utf8
        }
    }
}
Dismount-DiskImage -ImagePath "G:\csmw10.VHD"
"Done." | Out-File $out -Append -Encoding utf8
