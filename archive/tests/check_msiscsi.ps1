$out = "C:\Users\Admin\Desktop\cobdiskless\check_msiscsi_out.txt"
"Starting..." | Out-File $out -Encoding utf8
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
    $nt = "$winDrive\Windows\System32\ntoskrnl.exe"
    "ntoskrnl.exe ver: $((Get-Item $nt).VersionInfo.FileVersion)" | Out-File $out -Append -Encoding utf8

    Get-ChildItem -Path "$winDrive\Windows\WinSxS" -Recurse -Include *msiscsi* -ErrorAction SilentlyContinue | ForEach-Object {
        "WinSxS match: $($_.FullName)" | Out-File $out -Append -Encoding utf8
    }

    Get-ChildItem -Path "$winDrive\Windows\System32\DriverStore\FileRepository" -Recurse -Include *msiscsi* -ErrorAction SilentlyContinue | ForEach-Object {
        "DriverStore match: $($_.FullName)" | Out-File $out -Append -Encoding utf8
    }
}
Dismount-DiskImage -ImagePath "G:\csmw10.VHD"
"Done." | Out-File $out -Append -Encoding utf8
