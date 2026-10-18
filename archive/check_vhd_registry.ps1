# Self-elevate if not admin
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]$identity
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    $argList = '-NoProfile -ExecutionPolicy Bypass -File "' + $MyInvocation.MyCommand.Path + '"'
    $proc = Start-Process -FilePath "powershell.exe" -Verb RunAs -ArgumentList $argList -Wait -PassThru
    exit $proc.ExitCode
}

$logPath = Join-Path $PSScriptRoot "check_vhd_registry.log"
Start-Transcript -Path $logPath -Force

# Check VHD registry
$vhd = 'G:\G\csmw10.VHD'
Mount-DiskImage -ImagePath $vhd -StorageType VHD -Access ReadOnly | Out-Null
Start-Sleep -Seconds 2
$d = Get-DiskImage -ImagePath $vhd | Get-Disk
$parts = Get-Partition -DiskNumber $d.Number | Where-Object { $_.Type -ne 'Reserved' }
$vLetter = $null
foreach ($p in $parts) {
    if ($p.DriveLetter) {
        $candidate = "$($p.DriveLetter):"
        if (Test-Path "$candidate\Windows\System32\config\SYSTEM") {
            $vLetter = $candidate
            break
        }
    } else {
        Set-Partition -DiskNumber $d.Number -PartitionNumber $p.PartitionNumber -NewDriveLetter Y -ErrorAction SilentlyContinue
        if (Test-Path "Y:\Windows\System32\config\SYSTEM") {
            $vLetter = "Y:"
            break
        }
    }
}
Write-Host "Windows root: $vLetter"
if (-not $vLetter) {
    Dismount-DiskImage -ImagePath $vhd -ErrorAction SilentlyContinue | Out-Null
    exit 1
}

& reg.exe load HKLM\CHECK_SYS "$vLetter\Windows\System32\config\SYSTEM" | Out-Null

$services = @('msiscsi', 'Tcpip', 'Tcpip6', 'ndis', 'iScsiPrt', 'E1G60', 'e1iexpress', 'RTL8023x64', 'Rt640x64')
foreach ($s in $services) {
    Write-Host "=== Service: $s ==="
    & reg.exe query "HKLM\CHECK_SYS\ControlSet001\Services\$s" /v Start 2>&1
    & reg.exe query "HKLM\CHECK_SYS\ControlSet001\Services\$s" /v Group 2>&1
    & reg.exe query "HKLM\CHECK_SYS\ControlSet001\Services\$s" /v Tag 2>&1
    & reg.exe query "HKLM\CHECK_SYS\ControlSet001\Services\$s" /v ErrorControl 2>&1
    & reg.exe query "HKLM\CHECK_SYS\ControlSet001\Services\$s" /v ImagePath 2>&1
}

Write-Host "=== GroupOrderList SCSI miniport ==="
& reg.exe query "HKLM\CHECK_SYS\ControlSet001\Control\GroupOrderList" /v "SCSI miniport" 2>&1

Write-Host "=== GroupOrderList NDIS ==="
& reg.exe query "HKLM\CHECK_SYS\ControlSet001\Control\GroupOrderList" /v "NDIS" 2>&1

Write-Host "=== ServiceGroupOrder List ==="
& reg.exe query "HKLM\CHECK_SYS\ControlSet001\Control\ServiceGroupOrder" /v "List" 2>&1

Write-Host "=== Query Enum ROOT ISCSIPRT ==="
& reg.exe query "HKLM\CHECK_SYS\ControlSet001\Enum\ROOT\ISCSIPRT" /s 2>&1

Write-Host "=== Query Class SCSIAdapter all subkeys ==="
& reg.exe query "HKLM\CHECK_SYS\ControlSet001\Control\Class\{4d36e97b-e325-11ce-bfc1-08002be10318}" /s 2>&1

Write-Host "=== Query Disk/Class UpperFilters and LowerFilters ==="
& reg.exe query "HKLM\CHECK_SYS\ControlSet001\Control\Class\{4d36e967-e325-11ce-bfc1-08002be10318}" 2>&1
& reg.exe query "HKLM\CHECK_SYS\ControlSet001\Control\Class\{4d36e972-e325-11ce-bfc1-08002be10318}" 2>&1
& reg.exe query "HKLM\CHECK_SYS\ControlSet001\Control\Class\{4d36e97b-e325-11ce-bfc1-08002be10318}" 2>&1

Write-Host "=== Check Driver PE Architecture ==="
$checkFiles = @("E1G6032E.sys", "e1i63x64.sys", "Rtnic64.sys", "rt640x64.sys", "msiscsi.sys")
foreach ($f in $checkFiles) {
    $full = "$vLetter\Windows\System32\drivers\$f"
    if (Test-Path $full) {
        $bytes = [System.IO.File]::ReadAllBytes($full)
        $peOffset = [BitConverter]::ToInt32($bytes, 0x3c)
        $mach = [BitConverter]::ToUInt16($bytes, $peOffset + 4)
        $hex = $mach.ToString("X4")
        Write-Host "$f Machine: 0x$hex (8664=x64, 014C=x86)"
    } else {
        Write-Host "$f NOT FOUND"
    }
}

[GC]::Collect()
[GC]::WaitForPendingFinalizers()
& reg.exe unload HKLM\CHECK_SYS | Out-Null
Dismount-DiskImage -ImagePath $vhd -ErrorAction SilentlyContinue | Out-Null
Stop-Transcript
