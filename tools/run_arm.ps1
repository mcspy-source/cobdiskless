$armExe = "C:\Users\Admin\Desktop\cobecosystem\cobdiskless\build\bin\cob_armer.exe"
$vhd = "G:\G\csmw10.VHD"
$logFile = "C:\Users\Admin\Desktop\cobecosystem\cobdiskless\tools\arm_run.log"
$marker = "C:\Users\Admin\Desktop\cobecosystem\cobdiskless\tools\arm_run_done.marker"

if (Test-Path $marker) { Remove-Item $marker -Force -ErrorAction SilentlyContinue }
if (Test-Path $logFile) { Remove-Item $logFile -Force -ErrorAction SilentlyContinue }

$supScript = "$env:LOCALAPPDATA\Temp\opencode\arm_elevated.ps1"
$supDir = Split-Path -Parent $supScript
if (-not (Test-Path $supDir)) { New-Item -ItemType Directory -Path $supDir -Force | Out-Null }

$supContent = @"
`$vhd = '$vhd'
`$armExe = '$armExe'
`$logFile = '$logFile'
`$marker = '$marker'

attrib -r `$vhd
& `$armExe arm `$vhd > `$logFile 2>&1
`$rc = `$LASTEXITCODE
Set-Content -Path `$marker -Value `$rc -Force
"@
Set-Content -Path $supScript -Value $supContent -Force

$proc = Start-Process -FilePath "powershell.exe" -ArgumentList @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $supScript) -Verb RunAs -WindowStyle Hidden -PassThru

$sw = [System.Diagnostics.Stopwatch]::StartNew()
while ($sw.ElapsedMilliseconds -lt 60000) {
    if (Test-Path $marker) {
        Write-Host "Arming completed with exit code: $(Get-Content $marker -Raw)"
        break
    }
    Start-Sleep -Milliseconds 500
}

if (Test-Path $logFile) {
    Get-Content $logFile
} else {
    Write-Warning "Log file not created yet."
}
