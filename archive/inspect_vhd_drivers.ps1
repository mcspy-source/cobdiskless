# Self-elevate if not admin
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]$identity
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    $argList = '-NoProfile -ExecutionPolicy Bypass -File "' + $MyInvocation.MyCommand.Path + '"'
    $proc = Start-Process -FilePath "powershell.exe" -Verb RunAs -ArgumentList $argList -Wait -PassThru
    exit $proc.ExitCode
}

$logPath = Join-Path $PSScriptRoot "inspect_vhd_drivers.log"
Start-Transcript -Path $logPath -Force

$vhd = "G:\G\csmw10.VHD"
attrib -r $vhd
Mount-DiskImage -ImagePath $vhd -StorageType VHD -Access ReadOnly | Out-Null
Start-Sleep -Seconds 2
$d = Get-DiskImage -ImagePath $vhd | Get-Disk
$parts = Get-Partition -DiskNumber $d.Number | Where-Object { $_.Type -ne 'Reserved' }
$vLetter = $null
foreach ($p in $parts) {
    if ($p.DriveLetter) {
        $testPath = "$($p.DriveLetter):\Windows\System32\config\SYSTEM"
        if (Test-Path $testPath) { $vLetter = "$($p.DriveLetter):"; break }
    } else {
        Set-Partition -DiskNumber $d.Number -PartitionNumber $p.PartitionNumber -NewDriveLetter Y -ErrorAction SilentlyContinue
        if (Test-Path "Y:\Windows\System32\config\SYSTEM") { $vLetter = "Y:"; break }
    }
}

Write-Host "Windows root: $vLetter"
if ($vLetter) {
    Write-Host "Drivers in System32\drivers matching net/rt/e1:"
    Get-ChildItem "$vLetter\Windows\System32\drivers" -Filter "*.sys" | Where-Object { $_.Name -match 'e1|rt|net' } | Select-Object Name, Length | Format-Table -AutoSize
    
    Write-Host "Searching DriverStore INF files for hardware matches:"
    $infs = Get-ChildItem -Path "$vLetter\Windows\System32\DriverStore\FileRepository" -Filter "*.inf" -Recurse
    foreach ($pat in @("10D3", "100E", "8139", "8168")) {
        $hits = $infs | Select-String -Pattern "DEV_$pat" -SimpleMatch | Select-Object -First 3
        if ($hits) {
            Write-Host "=== Pattern DEV_$pat found in: ===" -ForegroundColor Green
            $hits | ForEach-Object { Write-Host "   $($_.Path) : $($_.Line.Trim())" }
        } else {
            Write-Host "=== Pattern DEV_$pat NOT found in DriverStore ===" -ForegroundColor Red
        }
    }
}

Dismount-DiskImage -ImagePath $vhd -ErrorAction SilentlyContinue | Out-Null
attrib +r $vhd
Stop-Transcript
