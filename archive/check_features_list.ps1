# Self-elevate if not admin
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]$identity
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    $argList = '-NoProfile -ExecutionPolicy Bypass -File "' + $MyInvocation.MyCommand.Path + '"'
    $proc = Start-Process -FilePath "powershell.exe" -Verb RunAs -ArgumentList $argList -Wait -PassThru
    exit $proc.ExitCode
}

$logPath = Join-Path $PSScriptRoot "check_features_list.log"
Start-Transcript -Path $logPath -Force

Get-WindowsOptionalFeature -Online | Where-Object { $_.State -eq 'Enabled' } | Select-Object -ExpandProperty FeatureName | Out-File (Join-Path $PSScriptRoot "enabled_features.txt")
Get-WindowsOptionalFeature -Online | Where-Object { $_.FeatureName -match 'Hyper|VirtualPlatform|Containers' } | Format-Table FeatureName, State -AutoSize

Stop-Transcript
