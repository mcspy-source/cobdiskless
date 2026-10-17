$out = "C:\Users\Admin\Desktop\cobdiskless\dism_restore.txt"
"Mounting VHD..." | Out-File $out -Encoding utf8
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
    "Running DISM RestoreHealth..." | Out-File $out -Append -Encoding utf8
    & dism.exe /Image:$winDrive /Cleanup-Image /RestoreHealth 2>&1 | Out-File $out -Append -Encoding utf8
    "Exit code: $LASTEXITCODE" | Out-File $out -Append -Encoding utf8
}
Dismount-DiskImage -ImagePath "G:\csmw10.VHD"
"Done." | Out-File $out -Append -Encoding utf8
