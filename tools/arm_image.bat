@echo off
setlocal
set "SCRIPT_DIR=%~dp0"
set "BIN_ARMER=%SCRIPT_DIR%..\build\bin\cob_armer.exe"
if not exist "%BIN_ARMER%" set "BIN_ARMER=%SCRIPT_DIR%..\release\cob_armer.exe"
if not exist "%BIN_ARMER%" set "BIN_ARMER=%SCRIPT_DIR%..\bin\cob_armer.exe"

if "%~1"=="" (
    echo =========================================================
    echo  CobDiskless Explicit 1-Click Image Arming Tool
    echo =========================================================
    echo Usage: arm_image.bat ^<path_to_vhd_or_hive^>
    echo Example: arm_image.bat G:\G\csmw10.VHD
    echo.
    exit /b 1
)

if not exist "%BIN_ARMER%" (
    echo [ERROR] cob_armer.exe binary not found. Please build the project first.
    exit /b 1
)

echo [INFO] Running explicit image arming on: "%~1"
"%BIN_ARMER%" arm "%~1"
exit /b %ERRORLEVEL%
