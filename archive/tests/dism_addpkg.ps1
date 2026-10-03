$out = "C:\Users\Admin\Desktop\cobdiskless\dism_addpkg.txt"
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
    "Running DISM Add-Package..." | Out-File $out -Append -Encoding utf8
    & dism.exe /Image:$winDrive /Add-Package /PackagePath:"C:\Users\Admin\Desktop\cobdiskless\kb4580390.msu" 2>&1 | Out-File $out -Append -Encoding utf8
    "DISM Exit code: $LASTEXITCODE" | Out-File $out -Append -Encoding utf8

    $ms = "$winDrive\Windows\System32\drivers\msiscsi.sys"
    if (Test-Path $ms) {
        "msiscsi.sys after Add-Package: ver: $((Get-Item $ms).VersionInfo.FileVersion) length: $((Get-Item $ms).Length)" | Out-File $out -Append -Encoding utf8
    }
}
Dismount-DiskImage -ImagePath "G:\csmw10.VHD"
"Done." | Out-File $out -Append -Encoding utf8
