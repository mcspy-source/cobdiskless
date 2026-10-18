Set-Location "C:\Users\Admin\Desktop\cobecosystem\cobdiskless"
$env:COB_STATION_MAC = "6C4B902F61EB"
$env:COB_STATION_IP = "192.168.1.5"
$env:COB_DRIVER_POOL = "C:\Users\Admin\Desktop\cobecosystem\cobdiskless\build\bin\driver_pool"

& .\build\bin\cob_armer.exe arm G:\G\csmw10.vhd > arm_run_output.txt 2>&1
