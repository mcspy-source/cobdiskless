$before = Get-Volume
$m = Mount-DiskImage -ImagePath 'I:\csmw10.VHD' -PassThru
Start-Sleep -Seconds 1
$after = Get-Volume
$diff = $after | Where-Object { $before.UniqueId -notcontains $_.UniqueId }
$res = @()
$res += "New volumes mounted from VHD:"
$res += ($diff | Format-Table -AutoSize | Out-String)
$disk = $m | Get-Disk
$parts = $disk | Get-Partition
$res += "Partitions on VHD disk:"
$res += ($parts | Format-Table -AutoSize | Out-String)
Dismount-DiskImage -ImagePath 'I:\csmw10.VHD'
$res | Out-File -FilePath "C:\Users\Admin\Desktop\cobdiskless\vhd_vols.txt" -Encoding utf8
