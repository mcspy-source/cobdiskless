$out = "C:\Users\Admin\Desktop\cobdiskless\vhd_sx_proper.txt"
"Searching WinSxS properly..." | Out-File $out -Encoding utf8
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
    [System.IO.Directory]::EnumerateFiles("$winDrive\Windows\WinSxS", "*msiscsi*", [System.IO.SearchOption]::AllDirectories) | ForEach-Object {
        "Found file: $_ ($((Get-Item $_).Length) bytes)" | Out-File $out -Append -Encoding utf8
    }
    [System.IO.Directory]::EnumerateFiles("$winDrive\Windows\WinSxS", "*iscsi.inf*", [System.IO.SearchOption]::AllDirectories) | ForEach-Object {
        "Found inf: $_ ($((Get-Item $_).Length) bytes)" | Out-File $out -Append -Encoding utf8
    }
}
Dismount-DiskImage -ImagePath "G:\csmw10.VHD"
"Done." | Out-File $out -Append -Encoding utf8
