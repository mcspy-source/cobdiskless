$out = "C:\Users\Admin\Desktop\cobdiskless\restore_drivers_out.txt"
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
    # Check and restore msiscsi.sys
    $srcMsiscsi = Get-ChildItem -Path "$winDrive\Windows\System32\DriverStore\FileRepository" -Filter "msiscsi.sys" -Recurse | Select-Object -First 1
    if ($srcMsiscsi) {
        $dstMsiscsi = "$winDrive\Windows\System32\drivers\msiscsi.sys"
        Copy-Item -Path $srcMsiscsi.FullName -Destination $dstMsiscsi -Force
        "Restored msiscsi.sys from $($srcMsiscsi.FullName) (ver: $((Get-Item $dstMsiscsi).VersionInfo.FileVersion))" | Out-File $out -Append -Encoding utf8
    } else {
        "ERROR: Could not find msiscsi.sys in DriverStore!" | Out-File $out -Append -Encoding utf8
    }

    # Check ndis.sys and tcpip.sys
    foreach ($f in @("ndis.sys", "tcpip.sys")) {
        $cur = "$winDrive\Windows\System32\drivers\$f"
        if (Test-Path $cur) {
            "Current $f ver: $((Get-Item $cur).VersionInfo.FileVersion)" | Out-File $out -Append -Encoding utf8
        }
    }
}
Dismount-DiskImage -ImagePath "G:\csmw10.VHD"
"Done." | Out-File $out -Append -Encoding utf8
