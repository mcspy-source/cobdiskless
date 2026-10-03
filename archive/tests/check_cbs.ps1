$out = "C:\Users\Admin\Desktop\cobdiskless\cbs_out.txt"
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
    $cbs = "$winDrive\Windows\Logs\CBS\CBS.log"
    if (Test-Path $cbs) {
        Get-Content $cbs | Select-String "msiscsi" | Select-Object -Last 30 | ForEach-Object {
            $_.Line | Out-File $out -Append -Encoding utf8
        }
    }
}
Dismount-DiskImage -ImagePath "G:\csmw10.VHD"
