$out = "C:\Users\Admin\Desktop\cobdiskless\fix_bcd_out.txt"
"Mounting VHD..." | Out-File $out -Encoding utf8
$m = Mount-DiskImage -ImagePath "G:\csmw10.VHD" -PassThru
$letters = ($m | Get-Disk | Get-Partition | Get-Volume).DriveLetter | Where-Object { $_ }
$bootDrive = $null
foreach ($l in $letters) {
    if (Test-Path "${l}:\Boot\BCD") {
        $bootDrive = "${l}:"
        break
    }
}
"Boot drive with BCD: $bootDrive" | Out-File $out -Append -Encoding utf8

if ($bootDrive) {
    $bcd = "$bootDrive\Boot\BCD"
    & bcdedit.exe /store $bcd /set "{bootmgr}" nointegritychecks on 2>&1 | Out-File $out -Append -Encoding utf8
    & bcdedit.exe /store $bcd /set "{bootmgr}" testsigning on 2>&1 | Out-File $out -Append -Encoding utf8
    & bcdedit.exe /store $bcd /set "{default}" nointegritychecks on 2>&1 | Out-File $out -Append -Encoding utf8
    & bcdedit.exe /store $bcd /set "{default}" testsigning on 2>&1 | Out-File $out -Append -Encoding utf8
    & bcdedit.exe /store $bcd /set "{default}" bootstatuspolicy ignoreallfailures 2>&1 | Out-File $out -Append -Encoding utf8
    & bcdedit.exe /store $bcd /set "{default}" recoveryenabled no 2>&1 | Out-File $out -Append -Encoding utf8
    
    "Checking updated BCD:" | Out-File $out -Append -Encoding utf8
    & bcdedit.exe /store $bcd /enum "{default}" 2>&1 | Out-File $out -Append -Encoding utf8
}
Dismount-DiskImage -ImagePath "G:\csmw10.VHD"
"Done." | Out-File $out -Append -Encoding utf8
