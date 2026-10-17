$ErrorActionPreference = "Continue"
$target = "G:\G\csmw10.vhd"
$dpScript = "C:\Users\Admin\Desktop\cobecosystem\cobdiskless\mount.dp"

Set-ItemProperty -Path $target -Name IsReadOnly -Value $false

@"
select vdisk file="$target"
attach vdisk
select partition 2
assign letter=V
"@ | Out-File -FilePath $dpScript -Encoding ascii

& diskpart.exe /s $dpScript | Out-Null
& reg.exe load HKLM\CHECK_SYS V:\Windows\System32\config\SYSTEM | Out-Null
& reg.exe query HKLM\CHECK_SYS\ControlSet001\Services\iScsiPrt | Out-File -FilePath "C:\Users\Admin\Desktop\cobecosystem\cobdiskless\iscsiprt_svc.txt" -Encoding ascii
& reg.exe query HKLM\CHECK_SYS\ControlSet001\Services\msiscsi | Out-File -FilePath "C:\Users\Admin\Desktop\cobecosystem\cobdiskless\msiscsi_svc.txt" -Encoding ascii
& reg.exe unload HKLM\CHECK_SYS | Out-Null

@"
select vdisk file="$target"
detach vdisk
"@ | Out-File -FilePath $dpScript -Encoding ascii

& diskpart.exe /s $dpScript | Out-Null
Set-ItemProperty -Path $target -Name IsReadOnly -Value $true
