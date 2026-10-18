$vhd = "G:\G\csmw10.VHD"
$logFile = "C:\Users\Admin\Desktop\cobecosystem\cobdiskless\tools\check_bcd_now.log"
$marker = "C:\Users\Admin\Desktop\cobecosystem\cobdiskless\tools\check_bcd_now.marker"

if (Test-Path $marker) { Remove-Item $marker -Force -ErrorAction SilentlyContinue }
if (Test-Path $logFile) { Remove-Item $logFile -Force -ErrorAction SilentlyContinue }

$supScript = "$env:LOCALAPPDATA\Temp\opencode\check_bcd_now_run.ps1"
$supContent = @"
`$vhd = '$vhd'
`$logFile = '$logFile'
`$marker = '$marker'

Start-Transcript -Path `$logFile -Force
Mount-DiskImage -ImagePath `$vhd -Access ReadOnly | Out-Null
Start-Sleep -Seconds 1
`$d = Get-DiskImage -ImagePath `$vhd | Get-Disk
`$parts = Get-Partition -DiskNumber `$d.Number | Where-Object { `$_.Type -ne 'Reserved' }
foreach (`$p in `$parts) {
    if (`$p.DriveLetter) {
        `$testBcd = "`$(`$p.DriveLetter):\Boot\BCD"
        if (Test-Path `$testBcd) {
            Write-Host "Found BCD on `$(`$p.DriveLetter):"
            & bcdedit.exe /store `$testBcd /enum all
        }
    }
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
