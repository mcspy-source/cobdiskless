$env:COB_STATION_MAC = "6C4B902F61EB"
$env:COB_STATION_IP = "192.168.1.5"
$env:COB_DRIVER_POOL = "C:\Users\Admin\Desktop\cobecosystem\cobdiskless\build\bin\driver_pool"

$armer = "C:\Users\Admin\Desktop\cobecosystem\cobdiskless\build\bin\cob_armer.exe"
$target = "G:\G\csmw10.vhd"
$log = "C:\Users\Admin\Desktop\cobecosystem\cobdiskless\arm_output.log"

Set-Location "C:\Users\Admin\Desktop\cobecosystem\cobdiskless"

& $armer arm $target *>&1 | Tee-Object -FilePath $log
exit $LASTEXITCODE
