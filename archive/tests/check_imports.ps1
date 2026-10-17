$out = "C:\Users\Admin\Desktop\cobdiskless\check_pe_imports.txt"
$m = Mount-DiskImage -ImagePath "G:\csmw10.VHD" -PassThru
$letters = ($m | Get-Disk | Get-Partition | Get-Volume).DriveLetter | Where-Object { $_ }
$winDrive = $null
foreach ($l in $letters) {
    if (Test-Path "${l}:\Windows\System32\ntoskrnl.exe") {
        $winDrive = "${l}:\"
        break
    }
}
if ($winDrive) {
    $ntos = "$winDrive\Windows\System32\ntoskrnl.exe"
    "Found ntoskrnl at $ntos" | Out-File $out -Encoding utf8
    
    # Check imports of msiscsi.sys
    $ms = "$winDrive\Windows\System32\drivers\msiscsi.sys"
    "msiscsi.sys path: $ms" | Out-File $out -Append -Encoding utf8
}
Dismount-DiskImage -ImagePath "G:\csmw10.VHD"
