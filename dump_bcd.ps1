$ErrorActionPreference = "Continue"
$target = "G:\G\csmw10.vhd"
$dpScript = "C:\Users\Admin\Desktop\cobecosystem\cobdiskless\mount.dp"

Set-ItemProperty -Path $target -Name IsReadOnly -Value $false

@"
select vdisk file="$target"
attach vdisk
select partition 1
assign letter=V
"@ | Out-File -FilePath $dpScript -Encoding ascii

& diskpart.exe /s $dpScript | Out-Null

$bcdPath = "V:\Boot\BCD"
if (Test-Path $bcdPath) {
    & bcdedit.exe /store $bcdPath /enum all | Out-File -FilePath "C:\Users\Admin\Desktop\cobecosystem\cobdiskless\bcd_dump.txt" -Encoding ascii
} else {
    Write-Output "BCD not found at V:\Boot\BCD" | Out-File -FilePath "C:\Users\Admin\Desktop\cobecosystem\cobdiskless\bcd_dump.txt" -Encoding ascii
}

@"
select vdisk file="$target"
detach vdisk
"@ | Out-File -FilePath $dpScript -Encoding ascii

& diskpart.exe /s $dpScript | Out-Null
Set-ItemProperty -Path $target -Name IsReadOnly -Value $true
