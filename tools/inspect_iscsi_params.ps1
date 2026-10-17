$vhd = "G:\G\csmw10.VHD"
$logFile = "C:\Users\Admin\Desktop\cobecosystem\cobdiskless\tools\inspect_iscsi_params.log"
$marker = "C:\Users\Admin\Desktop\cobecosystem\cobdiskless\tools\inspect_iscsi_params.marker"

if (Test-Path $marker) { Remove-Item $marker -Force -ErrorAction SilentlyContinue }
if (Test-Path $logFile) { Remove-Item $logFile -Force -ErrorAction SilentlyContinue }

$supScript = "$env:LOCALAPPDATA\Temp\opencode\inspect_iscsi_params_run.ps1"
$supContent = @"
`$vhd = '$vhd'
`$logFile = '$logFile'
`$marker = '$marker'

Start-Transcript -Path `$logFile -Force
Mount-DiskImage -ImagePath `$vhd -Access ReadOnly | Out-Null
Start-Sleep -Seconds 1
`$d = Get-DiskImage -ImagePath `$vhd | Get-Disk
`$parts = Get-Partition -DiskNumber `$d.Number | Where-Object { `$_.Type -ne 'Reserved' }
`$vLetter = `$null
foreach (`$p in `$parts) {
    if (`$p.DriveLetter) {
        `$testPath = "`$(`$p.DriveLetter):\Windows\System32\config\SYSTEM"
        if (Test-Path `$testPath) { `$vLetter = "`$(`$p.DriveLetter):"; break }
    }
}

Write-Host "VHD Partition: `$vLetter"
if (`$vLetter) {
    & reg.exe load HKLM\INSPECT_SYS "`$vLetter\Windows\System32\config\SYSTEM"
    
    Write-Host "=== iScsiPrt Parameters ==="
    & reg.exe query "HKLM\INSPECT_SYS\ControlSet001\Services\iScsiPrt\Parameters"
    
    Write-Host "=== msiscsi Parameters ==="
    & reg.exe query "HKLM\INSPECT_SYS\ControlSet001\Services\msiscsi\Parameters"
    
    Write-Host "=== Disk TimeOutValue ==="
    & reg.exe query "HKLM\INSPECT_SYS\ControlSet001\Services\disk" /v "TimeOutValue"
    
    Write-Host "=== Tcpip Parameters ==="
    & reg.exe query "HKLM\INSPECT_SYS\ControlSet001\Services\Tcpip\Parameters" /v "MaxUserPort"
    & reg.exe query "HKLM\INSPECT_SYS\ControlSet001\Services\Tcpip\Parameters" /v "TcpTimedWaitDelay"
    
    [GC]::Collect()
    [GC]::WaitForPendingFinalizers()
    & reg.exe unload HKLM\INSPECT_SYS
}
Dismount-DiskImage -ImagePath `$vhd -ErrorAction SilentlyContinue | Out-Null
Stop-Transcript
Set-Content -Path `$marker -Value "DONE" -Force
"@
Set-Content -Path $supScript -Value $supContent -Force

Start-Process -FilePath "powershell.exe" -ArgumentList @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $supScript) -Verb RunAs -WindowStyle Hidden -PassThru | Out-Null

$sw = [System.Diagnostics.Stopwatch]::StartNew()
while ($sw.ElapsedMilliseconds -lt 30000) {
    if (Test-Path $marker) { break }
    Start-Sleep -Milliseconds 500
}

if (Test-Path $logFile) {
    Get-Content $logFile
}
