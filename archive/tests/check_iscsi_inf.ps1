$out = "C:\Users\Admin\Desktop\cobdiskless\iscsi_inf_out.txt"
$m = Mount-DiskImage -ImagePath "G:\csmw10.VHD" -PassThru
$letters = ($m | Get-Disk | Get-Partition | Get-Volume).DriveLetter | Where-Object { $_ }
$winDrive = $null
foreach ($l in $letters) {
    if (Test-Path "${l}:\Windows\System32") {
        $winDrive = "${l}:\"
        break
    }
}
if ($winDrive) {
    Get-ChildItem -Path "$winDrive\Windows\System32\DriverStore\FileRepository" -Filter "*iscsi*" | ForEach-Object {
        "Folder: $($_.FullName)" | Out-File $out -Append -Encoding utf8
        Get-ChildItem -Path $_.FullName | ForEach-Object {
            "  -> $($_.Name) ($($_.Length) bytes)" | Out-File $out -Append -Encoding utf8
        }
        $inf = Join-Path $_.FullName "iscsi.inf"
        if (Test-Path $inf) {
            Select-String -Path $inf -Pattern "DriverVer" | ForEach-Object {
                "  DriverVer line: $($_.Line)" | Out-File $out -Append -Encoding utf8
            }
        }
    }
}
Dismount-DiskImage -ImagePath "G:\csmw10.VHD"
