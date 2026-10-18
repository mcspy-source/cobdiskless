$ErrorActionPreference = "Continue"
$target = "G:\G\csmw10.vhd"
$dpScript = "C:\Users\Admin\Desktop\cobecosystem\cobdiskless\mount.dp"

Set-ItemProperty -Path $target -Name IsReadOnly -Value $false

@"
select vdisk file="$target"
attach vdisk
list partition
list volume
"@ | Out-File -FilePath $dpScript -Encoding ascii

& diskpart.exe /s $dpScript | Out-File -FilePath "C:\Users\Admin\Desktop\cobecosystem\cobdiskless\partitions.txt" -Encoding ascii

@"
select vdisk file="$target"
detach vdisk
"@ | Out-File -FilePath $dpScript -Encoding ascii

& diskpart.exe /s $dpScript | Out-Null
Set-ItemProperty -Path $target -Name IsReadOnly -Value $true
