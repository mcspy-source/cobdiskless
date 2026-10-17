# Self-elevate if not admin
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]$identity
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    $argList = '-NoProfile -ExecutionPolicy Bypass -File "' + $MyInvocation.MyCommand.Path + '"'
    $proc = Start-Process -FilePath "powershell.exe" -Verb RunAs -ArgumentList $argList -Wait -PassThru
    exit $proc.ExitCode
}

$logPath = Join-Path $PSScriptRoot "query_pci_enum.log"
Start-Transcript -Path $logPath -Force

$vhd = 'G:\G\csmw10.VHD'
Mount-DiskImage -ImagePath $vhd -StorageType VHD -Access ReadOnly | Out-Null
Start-Sleep -Seconds 2
$d = Get-DiskImage -ImagePath $vhd | Get-Disk
$parts = Get-Partition -DiskNumber $d.Number | Where-Object { $_.Type -ne 'Reserved' }
$vLetter = $null
foreach ($p in $parts) {
    if ($p.DriveLetter) {
        $candidate = "$($p.DriveLetter):"
        if (Test-Path "$candidate\Windows\System32\config\SYSTEM") { $vLetter = $candidate; break }
    } else {
        Set-Partition -DiskNumber $d.Number -PartitionNumber $p.PartitionNumber -NewDriveLetter Y -ErrorAction SilentlyContinue
        if (Test-Path "Y:\Windows\System32\config\SYSTEM") { $vLetter = "Y:"; break }
    }
}
Write-Host "Windows root: $vLetter"
& reg.exe load HKLM\CHECK_SYS "$vLetter\Windows\System32\config\SYSTEM" | Out-Null

Write-Host "=== Subkeys of Enum\PCI ==="
& reg.exe query "HKLM\CHECK_SYS\ControlSet001\Enum\PCI" | Select-String -Pattern "VEN_"

Write-Host "=== Subkeys of Control\CriticalDeviceDatabase ==="
& reg.exe query "HKLM\CHECK_SYS\ControlSet001\Control\CriticalDeviceDatabase" | Select-String -Pattern "pci|iscsi"

Write-Host "=== Search DriverDatabase for 100e ==="
& reg.exe query "HKLM\CHECK_SYS\DriverDatabase\DriverInfFiles" /s /f "100e" 2>$null | Select-Object -First 20

[GC]::Collect()
[GC]::WaitForPendingFinalizers()
& reg.exe unload HKLM\CHECK_SYS | Out-Null
Dismount-DiskImage -ImagePath $vhd -ErrorAction SilentlyContinue | Out-Null
Stop-Transcript

