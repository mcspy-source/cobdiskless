$vhd = "G:\G\csmw10.VHD"
$logFile = "C:\Users\Admin\Desktop\cobecosystem\cobdiskless\tools\check_rt_rev06.log"
$marker = "C:\Users\Admin\Desktop\cobecosystem\cobdiskless\tools\check_rt_rev06.marker"

if (Test-Path $marker) { Remove-Item $marker -Force -ErrorAction SilentlyContinue }
if (Test-Path $logFile) { Remove-Item $logFile -Force -ErrorAction SilentlyContinue }

$supScript = "$env:LOCALAPPDATA\Temp\opencode\check_rt_rev06_run.ps1"
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
    `$inf = "`$vLetter\Windows\System32\DriverStore\FileRepository\rt640x64.inf_amd64_d58b21d3a19feca8\rt640x64.inf"
    Write-Host "Searching REV_06 in `$inf..."
    Select-String -Path `$inf -Pattern 'REV_06'
    Write-Host "Searching 8168E or 8168F in `$inf..."
    Select-String -Path `$inf -Pattern '8168E|8168F' | Select-Object -First 20
    Write-Host "Searching generic 8168 lines in `$inf..."
    Select-String -Path `$inf -Pattern 'DEV_8168' | Select-Object -First 20
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
