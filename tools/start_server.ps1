$WorkspaceRoot = "C:\Users\Admin\Desktop\cobecosystem\cobdiskless"
$AppExe = "$WorkspaceRoot\build\bin\cobdiskless_app.exe"
$AppWorkDir = "$WorkspaceRoot\build\bin"

# Check if already running
$existing = Get-Process "cobdiskless_app" -ErrorAction SilentlyContinue
if ($existing) {
    Write-Host "[*] Stopping existing cobdiskless_app (PID $($existing.Id))..."
    Start-Process powershell -ArgumentList @('-NoProfile', '-Command', "Stop-Process -Name cobdiskless_app -Force") -Verb RunAs -Wait
    Start-Sleep -Seconds 1
}

Write-Host "[*] Launching elevated cobdiskless_app.exe directly with Minimized window..."
Start-Process -FilePath $AppExe -ArgumentList '--start-daemon --enable-trace-io' -WorkingDirectory $AppWorkDir -WindowStyle Minimized -Verb RunAs

$sw = [System.Diagnostics.Stopwatch]::StartNew()
$ready = $false
while ($sw.ElapsedMilliseconds -lt 20000) {
    $u67 = @(Get-NetUDPEndpoint -ErrorAction SilentlyContinue | Where-Object { $_.LocalPort -eq 67 }).Count -gt 0
    $t18 = @(Get-NetTCPConnection -ErrorAction SilentlyContinue | Where-Object { $_.State -eq 'Listen' -and $_.LocalPort -eq 18080 }).Count -gt 0
    $t32 = @(Get-NetTCPConnection -ErrorAction SilentlyContinue | Where-Object { $_.State -eq 'Listen' -and $_.LocalPort -eq 3260 }).Count -gt 0
    if ($u67 -and $t18 -and $t32) {
        $ready = $true
        break
    }
    Start-Sleep -Milliseconds 400
}

$p = Get-Process "cobdiskless_app" -ErrorAction SilentlyContinue | Select-Object -First 1
if ($ready -and $p) {
    Write-Host "[+] Server daemon active (PID $($p.Id))!" -ForegroundColor Green
} else {
    Write-Host "[-] Server daemon failed to bind ports within 20s" -ForegroundColor Red
}

Write-Host "Service Ports Status:"
Write-Host "  DHCP/TFTP UDP 67: $(@(Get-NetUDPEndpoint -ErrorAction SilentlyContinue | Where-Object { $_.LocalPort -eq 67 }).Count -gt 0)"
Write-Host "  HTTP     TCP 18080: $(@(Get-NetTCPConnection -ErrorAction SilentlyContinue | Where-Object { $_.State -eq 'Listen' -and $_.LocalPort -eq 18080 }).Count -gt 0)"
Write-Host "  iSCSI    TCP 3260: $(@(Get-NetTCPConnection -ErrorAction SilentlyContinue | Where-Object { $_.State -eq 'Listen' -and $_.LocalPort -eq 3260 }).Count -gt 0)"
