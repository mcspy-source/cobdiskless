@echo off
cd /d "C:\Users\Admin\Desktop\cobecosystem\cobdiskless"
echo Starting arming at %date% %time% > tools\arm_offline.log
call tools\arm_image.bat "G:\G\csmw10.VHD" >> tools\arm_offline.log 2>&1
set RC=%ERRORLEVEL%
echo ExitCode=%RC% >> tools\arm_offline.log
echo %RC% > tools\arm_offline_rc.txt
