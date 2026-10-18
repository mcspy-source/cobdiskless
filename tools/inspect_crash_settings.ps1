$vhd = "G:\G\csmw10.VHD"
$logFile = "C:\Users\Admin\Desktop\cobecosystem\cobdiskless\tools\inspect_crash_settings.log"
$marker = "C:\Users\Admin\Desktop\cobecosystem\cobdiskless\tools\inspect_crash_settings.marker"

if (Test-Path $marker) { Remove-Item $marker -Force -ErrorAction SilentlyContinue }
if (Test-Path $logFile) { Remove-Item $logFile -Force -ErrorAction SilentlyContinue }

$supScript = "$env:LOCALAPPDATA\Temp\opencode\inspect_crash_settings_run.ps1"
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
    
    Write-Host "=== 1. CrashControl ==="
    & reg.exe query "HKLM\INSPECT_SYS\ControlSet001\Control\CrashControl"
    
    Write-Host "=== 2. Memory Management (PagingFiles) ==="
    & reg.exe query "HKLM\INSPECT_SYS\ControlSet001\Control\Session Manager\Memory Management" /v "PagingFiles"
    & reg.exe query "HKLM\INSPECT_SYS\ControlSet001\Control\Session Manager\Memory Management" /v "ExistingPageFiles"
    
    Write-Host "=== 3. rt640x64 Service Parameters ==="
    & reg.exe query "HKLM\INSPECT_SYS\ControlSet001\Services\rt640x64" /s
    
    Write-Host "=== 4. Realtek PCI Enum keys ==="
    & reg.exe query "HKLM\INSPECT_SYS\ControlSet001\Enum\PCI" /f "8168"
    
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
