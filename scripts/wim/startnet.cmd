@echo off
setlocal EnableExtensions EnableDelayedExpansion

REM ===========================================================================
REM CobDiskless in-RAM WinPE hardware staging environment.
REM
REM Served as /static/wim/startnet.cmd inside the boot.wim built by
REM build_winpe.ps1. It runs with NO target VHD attached, so the client is
REM physically unable to corrupt its own OS disk while drivers are injected:
REM
REM   1. wpeinit                      bring the NIC up from WinPE media
REM   2. detect MAC                   identity for the provisioning API
REM   3. net use S: \\SRV\staging     driver pool + VHD files (retry)
REM   4. attach OS VHD safely         iSCSI first, SMB loopback second
REM   5. dism /Add-Driver             generic RTL8168/8125 + e1d/e1r/i225
REM   6. detach + unshare             leave the disk exactly as found
REM   7. POST /api/client/staged      server arms the SYSTEM hive -> READY
REM   8. wpeutil reboot               next PXE boot takes the SAN boot branch
REM
REM Configuration file: %SYSTEMDRIVE%\cobdiskless_stage.cfg (optional), or
REM <staging share>\boot.cfg discovered after the share is mapped.
REM ===========================================================================

REM --- tunables (override in cobdiskless_stage.cfg) --------------------------
set "SERVER_IP="
set "SERVER_PORT=18080"
set "STAGING_SHARE=staging"
set "STAGING_DRIVE=S"
set "OS_DRIVE=V"
set "VHD_FILE="
set "ISCSI_TARGET="
set "ISCSI_PORTAL="
set "MAX_ATTEMPTS=6"
set "RETRY_SECONDS=3"
set "DRIVER_POOL=driver_pool"

set "MAC="
set "SYSTEM_HIVE="
set "ATTACH_METHOD="
set "ASSIGNED_LETTERS="
set "NEW_DISK="
set "EXIT_CODE=1"

if exist "%SYSTEMDRIVE%\cobdiskless_stage.cfg" call "%SYSTEMDRIVE%\cobdiskless_stage.cfg"

echo [startnet] ============================================================
echo [startnet] CobDiskless hardware staging environment
echo [startnet] ============================================================

REM --- 1. networking ---------------------------------------------------------
echo [startnet] Initializing network (wpeinit)...
call wpeinit
if errorlevel 1 echo [startnet] WARNING: wpeinit returned an error, continuing anyway

if not defined SERVER_IP call :detect_server_ip
if not defined SERVER_IP goto :fatal_no_server
echo [startnet] Server: !SERVER_IP!:!SERVER_PORT!

REM --- 2. identity -----------------------------------------------------------
call :detect_mac
if errorlevel 1 goto :fatal_no_mac
echo [startnet] Client MAC: !MAC!

REM --- 3. staging share ------------------------------------------------------
call :map_share
if errorlevel 1 goto :fatal_no_share

if exist "!STAGING_DRIVE!:\boot.cfg" call :read_boot_cfg
if not defined VHD_FILE set "VHD_FILE=os\!MAC::=!.vhd"

REM --- 4. attach the OS disk -------------------------------------------------
call :attach_os_volume
if errorlevel 1 goto :fatal_no_volume
echo [startnet] OS volume on !OS_DRIVE!: via !ATTACH_METHOD! (SYSTEM hive !SYSTEM_HIVE!)

REM --- 5. inject generic NIC drivers ----------------------------------------
call :inject_drivers
set "EXIT_CODE=!ERRORLEVEL!"

REM --- 6. detach exactly what we attached ------------------------------------
call :detach_os_volume

REM --- 7. tell the server the hive is ready to be armed ----------------------
if not "!EXIT_CODE!"=="0" goto :staging_failed
call :report_staged
set "EXIT_CODE=!ERRORLEVEL!"

call :unmap_share

if not "!EXIT_CODE!"=="0" goto :staging_failed
echo [startnet] Staging complete. Rebooting for the SAN boot branch...
wpeutil reboot
goto :eof

:staging_failed
echo [startnet] FAILED with exit code !EXIT_CODE!
echo [startnet] Rebooting in 30 seconds...
ping -n 30 127.0.0.1 >nul
wpeutil reboot
goto :eof

REM ===========================================================================
REM Detection helpers
REM ===========================================================================

:detect_server_ip
REM The DHCP default gateway is the CobDiskless server in the standard layout.
route print > "%TEMP%\route_print.txt" 2>nul
for /f "usebackq tokens=3" %%G in (`findstr /r /c:"0\.0\.0\.0 .*0\.0\.0\.0" "%TEMP%\route_print.txt"`) do (
    if not defined SERVER_IP set "SERVER_IP=%%G"
)
if defined SERVER_IP echo [startnet] Server auto-detected from default route: !SERVER_IP!
exit /b 0

:detect_mac
ipconfig /all > "%TEMP%\ipconfig_all.txt" 2>nul
for /f "usebackq tokens=2 delims=:" %%A in (`findstr /i /c:"Physical Address" /c:"MAC Address" "%TEMP%\ipconfig_all.txt"`) do (
    set "CANDIDATE=%%A"
    for /f "tokens=* delims= " %%B in ("!CANDIDATE!") do set "CANDIDATE=%%B"
    set "CANDIDATE=!CANDIDATE:-=:!"
    if /i not "!CANDIDATE!"=="00:00:00:00:00:00" if not "!CANDIDATE!"=="" set "MAC=!CANDIDATE!"
)
if not defined MAC (
    echo [startnet] ERROR: could not detect a MAC address
    exit /b 1
)
exit /b 0

:read_boot_cfg
for /f "usebackq tokens=1,* delims==" %%A in ("!STAGING_DRIVE!:\boot.cfg") do (
    if /i "%%A"=="mac" if not defined VHD_FILE set "VHD_FILE=%%B"
    if /i "%%A"=="vhd" set "VHD_FILE=%%B"
    if /i "%%A"=="iscsi_target" set "ISCSI_TARGET=%%B"
    if /i "%%A"=="iscsi_portal" set "ISCSI_PORTAL=%%B"
)
exit /b 0

REM ===========================================================================
REM Share mapping with retry
REM ===========================================================================

:map_share
for /l %%A in (1,1,%MAX_ATTEMPTS%) do (
    net use !STAGING_DRIVE!: /delete /y >nul 2>&1
    net use !STAGING_DRIVE!: \\!SERVER_IP!\!STAGING_SHARE! /user:guest "" /y >nul 2>&1
    if not errorlevel 1 (
        echo [startnet] Mapped !STAGING_DRIVE!: to \\!SERVER_IP!\!STAGING_SHARE! ^(attempt %%A^)
        exit /b 0
    )
    echo [startnet] Share map attempt %%A failed, retrying in !RETRY_SECONDS!s...
    ping -n !RETRY_SECONDS! 127.0.0.1 >nul
)
echo [startnet] ERROR: unable to map \\!SERVER_IP!\!STAGING_SHARE!
exit /b 1

:unmap_share
net use !STAGING_DRIVE!: /delete /y >nul 2>&1
exit /b 0

REM ===========================================================================
REM OS volume attach
REM
REM A VHD attached straight off SMB can deadlock: Windows pages through the
REM file while DISM writes to it, and a slow or SMB-redirected path turns that
REM into a request storm against a file the initiator already holds open. So:
REM
REM   1. prefer the iSCSI initiator (the OS disk never touches SMB), then
REM   2. fall back to an SMB loopback attach with retry + verification.
REM ===========================================================================

:attach_os_volume
if defined ISCSI_TARGET (
    call :attach_via_iscsi
    if not errorlevel 1 exit /b 0
)
call :attach_via_smb_loopback
exit /b !ERRORLEVEL!

:attach_via_iscsi
echo [startnet] iSCSI attach of !ISCSI_TARGET! at !ISCSI_PORTAL!
iscsicli AddTargetPortal !ISCSI_PORTAL! 3260 > "%TEMP%\iscsi_add.txt" 2>&1
call :snapshot_disks

for /l %%A in (1,1,%MAX_ATTEMPTS%) do (
    iscsicli QuickLoginAt !ISCSI_TARGET! !ISCSI_PORTAL! > "%TEMP%\iscsi_login.txt" 2>&1
    if not errorlevel 1 (
        set "ATTACH_METHOD=iSCSI"
        call :pick_new_disk
        if not errorlevel 1 (
            call :assign_letters
            call :verify_system_hive
            if not errorlevel 1 exit /b 0
        )
    )
    echo [startnet] iSCSI login attempt %%A not ready yet, retrying in !RETRY_SECONDS!s...
    ping -n !RETRY_SECONDS! 127.0.0.1 >nul
)
set "ATTACH_METHOD="
exit /b 1

:attach_via_smb_loopback
echo [startnet] SMB loopback attach of !STAGING_DRIVE!:\!VHD_FILE!
if not exist "!STAGING_DRIVE!:\!VHD_FILE!" (
    echo [startnet] ERROR: VHD not found on the staging share: !VHD_FILE!
    exit /b 1
)

call :snapshot_disks
for /l %%A in (1,1,%MAX_ATTEMPTS%) do (
    call :diskpart_attach
    if not errorlevel 1 (
        set "ATTACH_METHOD=SMB loopback"
        call :pick_new_disk
        if not errorlevel 1 (
            call :assign_letters
            call :verify_system_hive
            if not errorlevel 1 exit /b 0
            call :release_letters
        )
        call :diskpart_detach
    )
    echo [startnet] VHD attach attempt %%A not ready yet, retrying in !RETRY_SECONDS!s...
    ping -n !RETRY_SECONDS! 127.0.0.1 >nul
)
set "ATTACH_METHOD="
exit /b 1

:diskpart_attach
(
    echo select vdisk file="!STAGING_DRIVE!:\!VHD_FILE!"
    echo attach vdisk
) > "%TEMP%\dp_attach.txt"
diskpart /s "%TEMP%\dp_attach.txt" > "%TEMP%\dp_attach_out.txt" 2>&1
if errorlevel 1 exit /b 1
exit /b 0

:diskpart_detach
if not defined VHD_FILE exit /b 0
(
    echo select vdisk file="!STAGING_DRIVE!:\!VHD_FILE!"
    echo detach vdisk
) > "%TEMP%\dp_detach.txt"
diskpart /s "%TEMP%\dp_detach.txt" > "%TEMP%\dp_detach_out.txt" 2>&1
exit /b 0

:snapshot_disks
echo list disk> "%TEMP%\dp_list.txt"
diskpart /s "%TEMP%\dp_list.txt" > "%TEMP%\disk_before.txt" 2>&1
exit /b 0

REM Identify the disk that appeared since :snapshot_disks.
:pick_new_disk
echo list disk> "%TEMP%\dp_list.txt"
diskpart /s "%TEMP%\dp_list.txt" > "%TEMP%\disk_after.txt" 2>&1
findstr /v /x /l /g:"%TEMP%\disk_before.txt" "%TEMP%\disk_after.txt" > "%TEMP%\disk_new.txt" 2>nul

set "NEW_DISK="
for /f "usebackq tokens=2" %%D in (`findstr /r /c:"Disk [0-9]" "%TEMP%\disk_new.txt"`) do (
    if not defined NEW_DISK set "NEW_DISK=%%D"
)
REM Fallback: the focused row carries a leading asterisk.
if not defined NEW_DISK (
    for /f "usebackq tokens=3" %%D in (`findstr /b /l /c:"*" "%TEMP%\disk_after.txt"`) do (
        if not defined NEW_DISK set "NEW_DISK=%%D"
    )
)
if not defined NEW_DISK (
    echo [startnet] ERROR: could not identify the attached disk
    exit /b 1
)
echo [startnet] Attached disk: !NEW_DISK!
exit /b 0

REM Give every partition of the attached disk a scratch letter so the OS
REM volume can be located without guessing partition indexes.
:assign_letters
set "ASSIGNED_LETTERS="
if not defined NEW_DISK exit /b 1

set /a _part=0
for %%L in (W Y Z U T R Q) do (
    set /a _part+=1
    (
        echo select disk !NEW_DISK!
        echo select partition !_part!
        echo assign letter=%%L
    ) > "%TEMP%\dp_assign.txt"
    diskpart /s "%TEMP%\dp_assign.txt" >nul 2>&1
    if not errorlevel 1 set "ASSIGNED_LETTERS=!ASSIGNED_LETTERS! %%L"
)
exit /b 0

:release_letters
if not defined ASSIGNED_LETTERS exit /b 0
if not defined NEW_DISK exit /b 0
echo select disk !NEW_DISK!> "%TEMP%\dp_remove.txt"
for %%L in (!ASSIGNED_LETTERS!) do (
    echo remove letter=%%L noerr>> "%TEMP%\dp_remove.txt"
)
diskpart /s "%TEMP%\dp_remove.txt" >nul 2>&1
set "ASSIGNED_LETTERS="
exit /b 0

:verify_system_hive
set "SYSTEM_HIVE="
for %%L in (W Y Z U T R Q) do (
    if exist "%%L:\Windows\System32\config\SYSTEM" if "!SYSTEM_HIVE!"=="" (
        set "SYSTEM_HIVE=%%L:\Windows\System32\config\SYSTEM"
        set "OS_DRIVE=%%L"
    )
)
if not defined SYSTEM_HIVE (
    echo [startnet] SYSTEM hive not visible yet
    exit /b 1
)
exit /b 0

REM ===========================================================================
REM Driver injection
REM ===========================================================================

:inject_drivers
if not defined SYSTEM_HIVE (
    echo [startnet] ERROR: no SYSTEM hive resolved, cannot inject drivers
    exit /b 1
)
if not exist "!STAGING_DRIVE!:\!DRIVER_POOL!" (
    echo [startnet] ERROR: driver pool missing on share: !DRIVER_POOL!
    exit /b 1
)

echo [startnet] Injecting generic NIC drivers into !OS_DRIVE!:\ ...
dism /Image:!OS_DRIVE!\ /Add-Driver /Driver:!STAGING_DRIVE!\!DRIVER_POOL! /Recurse /Quiet
if errorlevel 1 (
    echo [startnet] ERROR: dism /Add-Driver failed with exit code !ERRORLEVEL!
    exit /b 1
)
echo [startnet] Driver injection complete.
exit /b 0

REM ===========================================================================
REM Detach / cleanup
REM ===========================================================================

:detach_os_volume
call :release_letters
if /i "!ATTACH_METHOD!"=="SMB loopback" call :diskpart_detach
if /i "!ATTACH_METHOD!"=="iSCSI" (
    for /l %%A in (1,1,3) do (
        iscsicli LogoutFromTarget !ISCSI_TARGET! 00000000 1 0 >nul 2>&1
    )
)
set "NEW_DISK="
set "SYSTEM_HIVE="
set "ATTACH_METHOD="
exit /b 0

REM ===========================================================================
REM Report to the CobDiskless server
REM ===========================================================================

:report_staged
set "PAYLOAD=%TEMP%\staged.json"
> "!PAYLOAD!" echo {"mac": "!MAC!"}
set "REPLY="
echo [startnet] POST http://!SERVER_IP!:!SERVER_PORT!/api/client/staged
for /f "usebackq delims=" %%R in (`curl -s -S --max-time 60 -X POST "http://!SERVER_IP!:!SERVER_PORT!/api/client/staged" -H "Content-Type: application/json" --data-binary "@!PAYLOAD!" 2^>^&1`) do (
    echo [startnet] server replied: %%R
    set "REPLY=!REPLY! %%R"
)
if not defined REPLY (
    echo [startnet] ERROR: no response from the server
    exit /b 1
)
if "!REPLY:READY=!"=="!REPLY!" (
    echo [startnet] ERROR: server did not confirm READY: !REPLY!
    exit /b 1
)
echo [startnet] Server armed the offline SYSTEM hive - READY
exit /b 0

REM ===========================================================================
REM Fatal paths - keep the console readable before rebooting
REM ===========================================================================

:fatal_no_server
echo [startnet] FATAL: SERVER_IP could not be determined.
echo [startnet] Set SERVER_IP in %SYSTEMDRIVE%\cobdiskless_stage.cfg and retry.
goto :fatal_reboot

:fatal_no_mac
echo [startnet] FATAL: could not detect this client's MAC address.
goto :fatal_reboot

:fatal_no_share
echo [startnet] FATAL: could not map \\!SERVER_IP!\!STAGING_SHARE!.
goto :fatal_reboot

:fatal_no_volume
echo [startnet] FATAL: could not attach/locate the target OS volume.
call :unmap_share
goto :fatal_reboot

:fatal_reboot
echo [startnet] Rebooting in 30 seconds...
ping -n 30 127.0.0.1 >nul
wpeutil reboot
goto :eof
