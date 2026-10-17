@echo on
set LOG=C:\Users\Admin\Desktop\cobecosystem\cobdiskless\tools\apply_fixes.log
echo Starting boot fixes execution at %date% %time% > "%LOG%"

echo [1] Deleting PagingFiles and ExistingPageFiles >> "%LOG%" 2>&1
reg delete "HKLM\FIX_SYS\ControlSet001\Control\Session Manager\Memory Management" /v PagingFiles /f >> "%LOG%" 2>&1
reg delete "HKLM\FIX_SYS\ControlSet001\Control\Session Manager\Memory Management" /v ExistingPageFiles /f >> "%LOG%" 2>&1
reg add "HKLM\FIX_SYS\ControlSet001\Control\Session Manager\Memory Management" /v ClearPageFileAtShutdown /t REG_DWORD /d 0 /f >> "%LOG%" 2>&1

echo [2] Checking CrashControl and Timeout keys >> "%LOG%" 2>&1
reg query "HKLM\FIX_SYS\ControlSet001\Control\CrashControl" /v AutoReboot >> "%LOG%" 2>&1
reg query "HKLM\FIX_SYS\ControlSet001\Services\disk" /v TimeOutValue >> "%LOG%" 2>&1

echo [3] Unloading HKLM\FIX_SYS >> "%LOG%" 2>&1
reg unload HKLM\FIX_SYS >> "%LOG%" 2>&1
if errorlevel 1 (
    echo Unload failed, waiting 2 seconds and retrying >> "%LOG%" 2>&1
    timeout /t 2 /nobreak >nul
    reg unload HKLM\FIX_SYS >> "%LOG%" 2>&1
)

echo [4] Sanitizing BCD entries on Y:\Boot\BCD >> "%LOG%" 2>&1
bcdedit /store Y:\Boot\BCD /set {bootmgr} device boot >> "%LOG%" 2>&1
bcdedit /store Y:\Boot\BCD /set {default} device boot >> "%LOG%" 2>&1
bcdedit /store Y:\Boot\BCD /set {default} osdevice boot >> "%LOG%" 2>&1
bcdedit /store Y:\Boot\BCD /set {default} sos yes >> "%LOG%" 2>&1
bcdedit /store Y:\Boot\BCD /set {default} bootlog yes >> "%LOG%" 2>&1
bcdedit /store Y:\Boot\BCD /set {default} bootstatuspolicy ignoreallfailures >> "%LOG%" 2>&1
bcdedit /store Y:\Boot\BCD /set {default} recoveryenabled no >> "%LOG%" 2>&1

echo [5] Enumerating BCD default entry >> "%LOG%" 2>&1
bcdedit /store Y:\Boot\BCD /enum {default} >> "%LOG%" 2>&1

echo [6] Dismounting VHD G:\G\csmw10.VHD >> "%LOG%" 2>&1
powershell -NoProfile -Command "Dismount-DiskImage -ImagePath 'G:\G\csmw10.VHD' -ErrorAction SilentlyContinue" >> "%LOG%" 2>&1

echo [7] Setting read-only attribute on VHD >> "%LOG%" 2>&1
attrib +r "G:\G\csmw10.VHD" >> "%LOG%" 2>&1

echo [8] Purging writeback overlay files >> "%LOG%" 2>&1
del /f /q "E:\wb_6c4b902f61eb.cow" >> "%LOG%" 2>&1
del /f /q "E:\writeback\wb_6c4b902f61eb.cow" >> "%LOG%" 2>&1
del /f /q "C:\Users\Admin\Desktop\cobecosystem\cobdiskless\build\bin\writeback\wb_6c4b902f61eb.cow" >> "%LOG%" 2>&1

echo ALL DONE >> "%LOG%" 2>&1
echo DONE > C:\Users\Admin\Desktop\cobecosystem\cobdiskless\tools\apply_fixes.marker
