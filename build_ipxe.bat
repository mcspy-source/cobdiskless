@echo off
set "PATH=C:\msys64\usr\bin;%PATH%"
cd /d "%~dp0third_party\ipxe\src"
echo Compiling undionly.kpxe with embedded CobDiskless script...
make bin/undionly.kpxe EMBED=embed_cob.ipxe -j4
if %ERRORLEVEL% EQU 0 (
    echo Build succeeded! Copying to tftp_boot...
    copy /Y bin\undionly.kpxe ..\..\..\tftp_boot\undionly.kpxe
    echo Done!
) else (
    echo Build failed with error %ERRORLEVEL%
)
