$out = "C:\Users\Admin\Desktop\cobdiskless\vhd_all_msiscsi.txt"
"Searching all msiscsi in VHD..." | Out-File $out -Encoding utf8
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
    Get-ChildItem -Path "$winDrive\" -Filter "*msiscsi*" -Recurse -Force -ErrorAction SilentlyContinue | ForEach-Object {
        "Found: $($_.FullName) ($($_.Length) bytes) modified: $($_.LastWriteTime) ver: $((Get-Item $_.FullName).VersionInfo.FileVersion)" | Out-File $out -Append -Encoding utf8
    }
}
Dismount-DiskImage -ImagePath "G:\csmw10.VHD"
"Done." | Out-File $out -Append -Encoding utf8
