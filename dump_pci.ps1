$ErrorActionPreference = "Continue"
$target = "G:\G\csmw10.vhd"
$log = "C:\Users\Admin\Desktop\cobecosystem\cobdiskless\dump_log.txt"
"Starting dump with diskpart..." | Out-File -FilePath $log -Encoding ascii

try {
    Set-ItemProperty -Path $target -Name IsReadOnly -Value $false
    "Cleared ReadOnly" | Out-File -FilePath $log -Append -Encoding ascii
    
    $dpScript = "C:\Users\Admin\Desktop\cobecosystem\cobdiskless\mount.dp"
    @"
select vdisk file="$target"
attach vdisk
select partition 1
assign letter=W
"@ | Out-File -FilePath $dpScript -Encoding ascii
    
    & diskpart.exe /s $dpScript | Out-File -FilePath $log -Append -Encoding ascii
    
    $sys = "W:\Windows\System32\config\SYSTEM"
    if (-not (Test-Path $sys)) {
        # Check other drives or partition 2
        @"
select vdisk file="$target"
select partition 2
assign letter=W
"@ | Out-File -FilePath $dpScript -Encoding ascii
        & diskpart.exe /s $dpScript | Out-File -FilePath $log -Append -Encoding ascii
    }
    
    "SYSTEM hive path: $sys (exists: $(Test-Path $sys))" | Out-File -FilePath $log -Append -Encoding ascii
    
    & reg.exe load HKLM\CHECK_SYS $sys
    "Loaded hive into HKLM\CHECK_SYS" | Out-File -FilePath $log -Append -Encoding ascii
    
    Get-ChildItem "HKLM:\CHECK_SYS\ControlSet001\Enum\PCI" | ForEach-Object {
        $devName = $_.PSChildName
        Get-ChildItem $_.PSPath | ForEach-Object {
            "$devName\$($_.PSChildName)"
        }
    } | Out-File -FilePath "C:\Users\Admin\Desktop\cobecosystem\cobdiskless\pci_enum.txt" -Encoding ascii
    
    & reg.exe query "HKLM\CHECK_SYS\ControlSet001\Control\Class\{4d36e972-e325-11ce-bfc1-08002be10318}" /s | Out-File -FilePath "C:\Users\Admin\Desktop\cobecosystem\cobdiskless\net_class.txt" -Encoding ascii
    & reg.exe query "HKLM\CHECK_SYS\ControlSet001\Services\Rt640x64" /s | Out-File -FilePath "C:\Users\Admin\Desktop\cobecosystem\cobdiskless\rt640x64_svc.txt" -Encoding ascii
    & reg.exe query "HKLM\CHECK_SYS\ControlSet001\Control\NetworkSetup2" /s | Out-File -FilePath "C:\Users\Admin\Desktop\cobecosystem\cobdiskless\ns2.txt" -Encoding ascii

    & reg.exe unload HKLM\CHECK_SYS
    "Unloaded hive" | Out-File -FilePath $log -Append -Encoding ascii
    
    @"
select vdisk file="$target"
detach vdisk
"@ | Out-File -FilePath $dpScript -Encoding ascii
    & diskpart.exe /s $dpScript | Out-File -FilePath $log -Append -Encoding ascii
    
    Set-ItemProperty -Path $target -Name IsReadOnly -Value $true
    "Re-locked VHD to ReadOnly" | Out-File -FilePath $log -Append -Encoding ascii
} catch {
    "ERROR: $_" | Out-File -FilePath $log -Append -Encoding ascii
}
