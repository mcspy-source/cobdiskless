$ErrorActionPreference = "Continue"
Write-Host "Arming G:\G\csmw10.VHD..."
& "C:\Users\Admin\Desktop\cobecosystem\cobdiskless\build\bin\cob_armer.exe" arm "G:\G\csmw10.VHD" *>&1 | Set-Content -Path "C:\Users\Admin\Desktop\cobecosystem\cobdiskless\logs\arm_output2.log" -Encoding UTF8
Write-Host "cob_armer exited with code: $LASTEXITCODE"
