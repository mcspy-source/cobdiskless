$m = Mount-DiskImage -ImagePath 'G:\csmw10.VHD' -PassThru
$disk = $m | Get-Disk
$parts = $disk | Get-Partition
$vols = Get-Volume
$res = @()
$res += ($parts | Format-Table -AutoSize | Out-String)
$res += ($vols | Format-Table -AutoSize | Out-String)
Dismount-DiskImage -ImagePath 'G:\csmw10.VHD'
$res | Out-File -FilePath "C:\Users\Admin\Desktop\cobdiskless\parts_check.txt" -Encoding utf8
