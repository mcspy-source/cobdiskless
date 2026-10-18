@echo off
rem Launch CobDiskless Diskless Server Stack (DHCP + TFTP + iSCSI + HTTP)
cd /d "%~dp0"

net session >nul 2>&1
if %errorlevel% neq 0 (
    echo [INFO] Administrator privileges required for network sockets and image arming. Elevating...
    powershell -NoProfile -ExecutionPolicy Bypass -Command "Start-Process cmd -ArgumentList '/k cd /d \"\"%~dp0\"\" && start_server.bat' -Verb RunAs"
    exit /b
)

"cobdiskless_app.exe" --start
pause
