@echo off
setlocal
rem ===========================================================================
rem  build_ipxe.bat -- build the four CobPXE firmware targets
rem
rem  Reproduces the exact command sequence verified to produce all four targets
rem  with zero build errors.  The BIOS and UEFI links are NOT interchangeable:
rem
rem    UEFI  passes EXTRA_LDFLAGS="-pie --no-dynamic-linker" so util/elf2efi.c
rem          can convert the ELF to PE without tripping an unhandled
rem          R_X86_64_32S relocation (GCC 13.2 relaxes the
rem          R_X86_64_REX_GOTPCRELX reference to __stack_chk_guard).  -pie is
rem          what makes that reference become PC32 instead.  See
rem          docs/THIRD_PARTY_NOTES.md section 7.
rem
rem    BIOS  must NOT receive -pie; it links exactly as before.
rem
rem  Embedded script: tftp_boot\boot.ipxe, referenced from third_party\ipxe\src
rem  as ..\..\..\tftp_boot\boot.ipxe.  (An earlier revision of this script used
rem  EMBED=embed_cob.ipxe, a file that does not exist anywhere in the tree, so
rem  the build could never have succeeded as written.)
rem
rem  Usage:
rem    build_ipxe.bat            clean build of all four, copy to tftp_boot\
rem    build_ipxe.bat noclean    reuse existing objects (faster iteration)
rem ===========================================================================

set "ROOT=%~dp0"
set "MSYS_BIN=C:\msys64\usr\bin"
set "CROSS_BIN=C:\msys64\opt\cross\bin"
set "EMBED=../../../tftp_boot/boot.ipxe"
set "RC=0"

rem Host tools (gcc, make) come from MSYS2; the cross toolchain from /opt/cross.
set "PATH=%CROSS_BIN%;%MSYS_BIN%;%PATH%"

cd /d "%ROOT%third_party\ipxe\src"
if errorlevel 1 (
    echo ERROR: cannot enter "%ROOT%third_party\ipxe\src"
    exit /b 1
)

echo ============================================================
echo  toolchain
echo ============================================================
for /f "delims=" %%V in ('x86_64-elf-gcc --version') do (echo  %%V & goto :tcok)
echo ERROR: x86_64-elf-gcc not found at %CROSS_BIN%
exit /b 1
:tcok
if not exist "%EMBED%" (
    echo ERROR: embed script not found: "%CD%\%EMBED%"
    exit /b 1
)
echo  embed: %EMBED%

echo.
echo ============================================================
echo  make clean
echo ============================================================
if /I "%~1"=="noclean" (
    echo  skipped (noclean)
) else (
    make clean
    if errorlevel 1 (
        echo ERROR: make clean failed
        exit /b 1
    )
)

echo.
echo ============================================================
echo  [1/4] bin/realtek.kpxe -- BIOS, plain link
echo ============================================================
make bin/realtek.kpxe CROSS=x86_64-elf- HOST_CC=gcc EMBED=%EMBED%
if errorlevel 1 goto :fail

echo.
echo ============================================================
echo  [2/4] bin/undionly.kpxe -- BIOS, plain link
echo ============================================================
make bin/undionly.kpxe CROSS=x86_64-elf- HOST_CC=gcc EMBED=%EMBED%
if errorlevel 1 goto :fail

echo.
echo ============================================================
echo  [3/4] bin-x86_64-efi/ipxe.efi -- UEFI, -pie, stack protector on
echo ============================================================
make bin-x86_64-efi/ipxe.efi CROSS=x86_64-elf- HOST_CC=gcc EMBED=%EMBED% EXTRA_LDFLAGS="-pie --no-dynamic-linker"
if errorlevel 1 goto :fail

echo.
echo ============================================================
echo  [4/4] bin-x86_64-efi/snponly.efi -- UEFI, -pie, stack protector on
echo ============================================================
make bin-x86_64-efi/snponly.efi CROSS=x86_64-elf- HOST_CC=gcc EMBED=%EMBED% EXTRA_LDFLAGS="-pie --no-dynamic-linker"
if errorlevel 1 goto :fail

echo.
echo ============================================================
echo  copy to tftp_boot\
echo ============================================================
copy /Y "bin\realtek.kpxe"           "%ROOT%tftp_boot\realtek.kpxe"    >nul || set RC=1
copy /Y "bin\undionly.kpxe"          "%ROOT%tftp_boot\undionly.kpxe"   >nul || set RC=1
copy /Y "bin-x86_64-efi\ipxe.efi"    "%ROOT%tftp_boot\ipxe.efi"        >nul || set RC=1
copy /Y "bin-x86_64-efi\snponly.efi" "%ROOT%tftp_boot\snponly.efi"     >nul || set RC=1
if not "%RC%"=="0" (
    echo ERROR: one or more copies failed
    exit /b 1
)

echo.
echo ============================================================
echo  outputs
echo ============================================================
set "RC=0"
if not exist "bin\realtek.kpxe"           (echo   MISSING  bin\realtek.kpxe           & set RC=1)
if not exist "bin\undionly.kpxe"          (echo   MISSING  bin\undionly.kpxe          & set RC=1)
if not exist "bin-x86_64-efi\ipxe.efi"    (echo   MISSING  bin-x86_64-efi\ipxe.efi    & set RC=1)
if not exist "bin-x86_64-efi\snponly.efi" (echo   MISSING  bin-x86_64-efi\snponly.efi & set RC=1)
for %%F in ("bin\realtek.kpxe" "bin\undionly.kpxe" "bin-x86_64-efi\ipxe.efi" "bin-x86_64-efi\snponly.efi") do (
    if exist %%F echo   %%~zF bytes  %%~F
)
echo.
if "%RC%"=="0" (
    echo BUILD OK -- all four targets built and copied to tftp_boot\.
) else (
    echo BUILD FAILED -- one or more targets are missing.
)
echo ============================================================
exit /b %RC%

:fail
echo.
echo ############################################################
echo #  BUILD FAILED -- aborting.  Earlier targets that succeeded
echo #  were NOT copied to tftp_boot\.
echo ############################################################
exit /b 1
