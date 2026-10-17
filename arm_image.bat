@echo off
setlocal
cd /d "%~dp0"

net session >nul 2>&1
if %errorlevel% neq 0 (
    echo [INFO] Requesting Administrator privileges...
    powershell -NoProfile -ExecutionPolicy Bypass -Command "Start-Process -FilePath '%~f0' -ArgumentList '%*' -Verb RunAs"
    exit /b
)

title CobDiskless Offline Image Armer

echo ===================================================================
echo       CobDiskless Offline Image Armer ^& Driver Sanitizer
echo ===================================================================
echo.
echo  This tool fixes the 0x0000007B BSOD by injecting BOOT_START (0)
echo  and BootFlags=0x1 into network drivers and msiscsi inside the VHD.
echo.

if "%~1"=="" (
    set /p TARGET="Enter target VHD path (e.g. .\images\csmw10.VHD): "
) else (
    set "TARGET=%~1"
)

rem Strip double quotes if user entered or dragged path
set "TARGET=%TARGET:"=%"

if not exist "%TARGET%" (
    echo.
    echo [ERROR] File does not exist: "%TARGET%"
    echo.
    pause
    exit /b 1
)

echo.
echo [INFO] Arming image: "%TARGET%" ...
echo.

if exist "release\cobdiskless_app.exe" (
    "release\cobdiskless_app.exe" --arm-image "%TARGET%"
) else if exist "build\bin\cobdiskless_app.exe" (
    "build\bin\cobdiskless_app.exe" --arm-image "%TARGET%"
) else if exist "cobdiskless_app.exe" (
    "cobdiskless_app.exe" --arm-image "%TARGET%"
) else (
    echo [ERROR] cobdiskless_app.exe not found!
)

echo.
pause
