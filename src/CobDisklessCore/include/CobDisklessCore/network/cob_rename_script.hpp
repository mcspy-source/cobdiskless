#pragma once

#include <string>
#include <string_view>

namespace cobdiskless::ipxe {

/**
 * @brief Placeholders substituted when the helper script is rendered.
 *
 * @c render_rename_script() replaces them with the live server address so the
 * generated file is ready to drop into the VHD's startup folder / RunOnce key.
 */
inline constexpr const char* kServerIpPlaceholder = "@SERVER_IP@";
inline constexpr const char* kHttpPortPlaceholder = "@HTTP_PORT@";

/**
 * @brief The Windows hostname provisioning helper (cob_rename.cmd).
 *
 * Deployed inside the golden image (startup folder, RunOnce, or a scheduled
 * task) so a freshly booted diskless client reconciles its own computer name
 * with the name the operator typed during iPXE enrollment:
 *
 *   1. read the first installed adapter's MAC address
 *   2. GET /api/whoami?mac=<MAC> from the CobDiskless server
 *   3. compare against %COMPUTERNAME% and exit early when they already match
 *   4. write ComputerName / ActiveComputerName and the Tcpip Hostname values,
 *      then perform the actual rename so the change survives a reboot
 *
 * Server address and port may still be overridden at runtime with the
 * COBDISKLESS_SERVER / COBDISKLESS_HTTP_PORT environment variables, which
 * keeps the same file usable across multiple server deployments.
 */
inline constexpr const char* kCobRenameScript = R"CMD(@echo off
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
if "%SERVER%"=="" set "SERVER=@SERVER_IP@"
set "PORT=%COBDISKLESS_HTTP_PORT%"
if "%PORT%"=="" set "PORT=@HTTP_PORT@"

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
)CMD";

/**
 * @brief Renders the helper script with the live endpoint baked in.
 *
 * @param server_ip  dotted-quad address of the CobDiskless server
 * @param http_port  TCP port of the embedded HTTP daemon
 */
[[nodiscard]] inline std::string render_rename_script(std::string_view server_ip,
                                                      unsigned int http_port) {
    std::string script;
    script.reserve(sizeof(kCobRenameScript));
    const std::string raw{kCobRenameScript};

    const auto replace_all = [](std::string& text,
                                std::string_view needle,
                                std::string_view replacement) {
        if (needle.empty()) {
            return;
        }
        size_t pos = 0;
        while ((pos = text.find(needle, pos)) != std::string::npos) {
            text.replace(pos, needle.size(), replacement);
            pos += replacement.size();
        }
    };

    script = raw;
    replace_all(script, kServerIpPlaceholder, server_ip);
    replace_all(script, kHttpPortPlaceholder, std::to_string(http_port));
    return script;
}

} // namespace cobdiskless::ipxe
