$m = Mount-DiskImage -ImagePath 'G:\csmw10.VHD' -PassThru
$disk = $m | Get-Disk
$parts = $disk | Get-Partition
$res = @()
$res += "disk number: $($disk.Number)"
$res += ($parts | Format-Table -AutoSize | Out-String)
Dismount-DiskImage -ImagePath 'G:\csmw10.VHD'
$res | Out-File -FilePath "C:\Users\Admin\Desktop\cobdiskless\vhd_state.txt" -Encoding utf8
