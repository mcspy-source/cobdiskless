@echo off
setlocal EnableExtensions EnableDelayedExpansion
REM ===========================================================================
REM  CobDiskless - Windows hostname provisioning helper (cob_rename.cmd)
REM
REM  Syncs this machine's Windows computer name with the hostname assigned by
REM  the CobDiskless enrollment service (/api/enroll -> clients.json).
REM
REM  Intended to run at logon / startup from inside the diskless VHD image.
REM  Must be executed elevated (the helper relaunches itself if needed).
REM
REM  This copy carries placeholder server/port tokens. The CobDiskless
REM  server rewrites cob_rename.cmd in its working directory on startup with
REM  the detected server address, and also serves the rendered copy at
REM  http://<server>:<http_port>/cob_rename.cmd. Use that rendered copy
REM  inside the golden image.
REM ===========================================================================

set "SERVER=%COBDISKLESS_SERVER%"
if "%SERVER%"=="" set "SERVER=192.168.1.99"
set "PORT=%COBDISKLESS_HTTP_PORT%"
if "%PORT%"=="" set "PORT=18080"

REM --- Re-launch elevated -----------------------------------------------------
net session >nul 2>&1
if errorlevel 1 (
    powershell -NoProfile -ExecutionPolicy Bypass -Command ^
        "Start-Process -FilePath '%~f0' -Verb RunAs -WindowStyle Hidden"
    exit /b 0
)

REM --- First installed adapter's MAC address ---------------------------------
set "MAC="
for /f "skip=1 tokens=1 delims=," %%A in ('getmac /fo csv /nh 2^>nul') do (
    if not defined MAC set "MAC=%%~A"
)
if not defined MAC (
    echo [cob_rename] Unable to determine the local MAC address.
    exit /b 1
)
set "MAC=%MAC::=-%"
set "MAC=%MAC:-=:%"

REM --- Ask the server who this machine should be -----------------------------
set "NEWNAME="
set "PS_TMP=%TEMP%\cob_rename_%RANDOM%.ps1"
> "%PS_TMP%" (
    echo $ErrorActionPreference = 'Stop'
    echo try {
    echo   $r = Invoke-RestMethod -Uri 'http://%SERVER%:%PORT%/api/whoami?mac=%MAC%' -TimeoutSec 10
    echo   Write-Output $r.hostname
    echo } catch {
    echo   Write-Output ''
    echo }
)
for /f "usebackq delims=" %%H in (`powershell -NoProfile -ExecutionPolicy Bypass -File "%PS_TMP%"`) do (
    if not defined NEWNAME set "NEWNAME=%%H"
)
del "%PS_TMP%" >nul 2>&1

if not defined NEWNAME (
    echo [cob_rename] Server http://%SERVER%:%PORT% did not return a hostname for %MAC%.
    exit /b 1
)

REM --- Already correct? -------------------------------------------------------
if /i "%NEWNAME%"=="%COMPUTERNAME%" (
    echo [cob_rename] Hostname already correct: %COMPUTERNAME%
    exit /b 0
)

echo [cob_rename] Renaming %COMPUTERNAME% -^> %NEWNAME%

REM --- Persist the new name in the registry ----------------------------------
reg add "HKLM\SYSTEM\CurrentControlSet\Control\ComputerName\ComputerName" ^
    /v ComputerName /t REG_SZ /d "%NEWNAME%" /f >nul 2>&1
reg add "HKLM\SYSTEM\CurrentControlSet\Control\ComputerName\ActiveComputerName" ^
    /v ComputerName /t REG_SZ /d "%NEWNAME%" /f >nul 2>&1
reg add "HKLM\SYSTEM\CurrentControlSet\Services\Tcpip\Parameters" ^
    /v Hostname /t REG_SZ /d "%NEWNAME%" /f >nul 2>&1
reg add "HKLM\SYSTEM\CurrentControlSet\Services\Tcpip\Parameters" ^
    /v "NV Hostname" /t REG_SZ /d "%NEWNAME%" /f >nul 2>&1

REM --- Perform the actual rename (takes effect after reboot) -----------------
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
    "try { Rename-Computer -NewName '%NEWNAME%' -Force -ErrorAction Stop } catch { }"

echo [cob_rename] Hostname set to %NEWNAME%. Reboot to finish.
exit /b 0
