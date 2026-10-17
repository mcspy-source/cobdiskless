$out = "C:\Users\Admin\Desktop\cobdiskless\dism_repair_source.txt"
"Mounting VHD..." | Out-File $out -Encoding utf8
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
    "Deleting corrupted/mismatched msiscsi.sys from drivers..." | Out-File $out -Append -Encoding utf8
    $cur = "$winDrive\Windows\System32\drivers\msiscsi.sys"
    if (Test-Path $cur) {
        Remove-Item -Path $cur -Force
    }

    "Running DISM RestoreHealth with local Source..." | Out-File $out -Append -Encoding utf8
    & dism.exe /Image:$winDrive /Cleanup-Image /RestoreHealth /Source:"C:\Users\Admin\Desktop\cobdiskless\kb4580390.msu" /LimitAccess 2>&1 | Out-File $out -Append -Encoding utf8
    "Exit code: $LASTEXITCODE" | Out-File $out -Append -Encoding utf8

    if (Test-Path $cur) {
        "msiscsi.sys restored: ver: $((Get-Item $cur).VersionInfo.FileVersion) length: $((Get-Item $cur).Length)" | Out-File $out -Append -Encoding utf8
    } else {
        "msiscsi.sys still not present in drivers dir" | Out-File $out -Append -Encoding utf8
    }
}
Dismount-DiskImage -ImagePath "G:\csmw10.VHD"
"Done." | Out-File $out -Append -Encoding utf8
