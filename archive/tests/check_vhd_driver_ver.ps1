$m = Mount-DiskImage -ImagePath "G:\csmw10.VHD" -PassThru
$parts = $m | Get-Disk | Get-Partition
$osPart = $parts | Where-Object { Test-Path "$($_.DriveLetter):\Windows\System32\drivers\rt640x64.sys" }
$letter = $osPart.DriveLetter
Write-Host "OS Drive Letter: $letter"

$drvPath = "$letter`:\Windows\System32\drivers\rt640x64.sys"
$ver = (Get-Item $drvPath).VersionInfo
$out = "Current rt640x64.sys in VHD: FileVersion=$($ver.FileVersion), ProductVersion=$($ver.ProductVersion), Length=$((Get-Item $drvPath).Length)"
Write-Host $out
$out | Out-File -FilePath "C:\Users\Admin\Desktop\cobdiskless\vhd_current_driver_ver.txt" -Encoding utf8

Dismount-DiskImage -ImagePath "G:\csmw10.VHD"
