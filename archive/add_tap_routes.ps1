$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]$identity
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    $argList = '-NoProfile -ExecutionPolicy Bypass -File "' + $MyInvocation.MyCommand.Path + '"'
    $proc = Start-Process -FilePath "powershell.exe" -Verb RunAs -ArgumentList $argList -Wait -PassThru
    exit $proc.ExitCode
}

Write-Host "Adding routes..."
& route.exe add 192.168.1.102 mask 255.255.255.255 192.168.1.99 metric 1 if 21
& route.exe add 192.168.1.103 mask 255.255.255.255 192.168.1.99 metric 1 if 21
Start-Sleep -Seconds 1
