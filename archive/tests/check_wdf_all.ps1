$m = Mount-DiskImage -ImagePath 'G:\csmw10.VHD' -PassThru
$vol = Get-Volume | Where-Object { $_.FileSystemLabel -eq "os" }
$sysPath = "$($vol.DriveLetter):\Windows\System32\config\SYSTEM"
& reg load HKLM\OFFLINE_SYS "$sysPath"
$res = @()
$res += (& reg query "HKLM\OFFLINE_SYS\ControlSet001\Services" | Select-String -Pattern "Wdf|wdf")
& reg unload HKLM\OFFLINE_SYS
Dismount-DiskImage -ImagePath 'G:\csmw10.VHD'
$res | Out-File -FilePath "C:\Users\Admin\Desktop\cobdiskless\wdf_all_check.txt" -Encoding utf8
