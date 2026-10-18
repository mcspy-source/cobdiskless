# Self-elevate if not admin
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]$identity
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    $argList = '-NoProfile -ExecutionPolicy Bypass -File "' + $MyInvocation.MyCommand.Path + '"'
    $proc = Start-Process -FilePath "powershell.exe" -Verb RunAs -ArgumentList $argList -Wait -PassThru
    exit $proc.ExitCode
}

$logPath = Join-Path $PSScriptRoot "check_ntbtlog.log"
Start-Transcript -Path $logPath -Force

$vhd = 'G:\G\csmw10.VHD'
Mount-DiskImage -ImagePath $vhd -StorageType VHD -Access ReadOnly | Out-Null
Start-Sleep -Seconds 2
$d = Get-DiskImage -ImagePath $vhd | Get-Disk
$parts = Get-Partition -DiskNumber $d.Number
$found = $false
foreach ($p in $parts) {
    if ($p.DriveLetter) {
        $cand = "$($p.DriveLetter):\Windows\ntbtlog.txt"
        if (Test-Path $cand) {
            Write-Host "ntbtlog found! Length: $((Get-Item $cand).Length)"
            Get-Content $cand | Select-Object -Last 40
            $found = $true
            break
        }
    } else {
        Set-Partition -DiskNumber $d.Number -PartitionNumber $p.PartitionNumber -NewDriveLetter Z -ErrorAction SilentlyContinue
        $cand = "Z:\Windows\ntbtlog.txt"
        if (Test-Path $cand) {
            Write-Host "ntbtlog found on Z:! Length: $((Get-Item $cand).Length)"
            Get-Content $cand | Select-Object -Last 40
            $found = $true
            break
        }
    }
}
if (-not $found) { Write-Host "ntbtlog.txt was not found." }
Dismount-DiskImage -ImagePath $vhd -ErrorAction SilentlyContinue | Out-Null
Stop-Transcript
