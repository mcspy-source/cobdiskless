# Realtek NIC diagnosis collector
# Run this on the machine showing the 0xC00102 error, as ADMIN.
#   powershell -ExecutionPolicy Bypass -File .\collect-rtk-diag.ps1
# Output: rtk-diag.txt next to this script. Nothing is modified.

$ErrorActionPreference = 'SilentlyContinue'
$out = Join-Path $PSScriptRoot 'rtk-diag.txt'
if (-not $PSScriptRoot) { $out = ".\rtk-diag.txt" }
function W($s) { $s | Tee-Object -FilePath $out -Append }

"=== collected $(Get-Date) ===" | Out-File $out

W "`n--- 1. NIC inventory (real hardware IDs) ---"
Get-NetAdapter | Select-Object Name, InterfaceDescription, Status, MacAddress, LinkSpeed |
    Format-Table -AutoSize | Out-String | Tee-Object -Append

W "`n--- 2. PnP device IDs for Realtek NICs ---"
Get-PnpDevice -Class Net | Where-Object { $_.InstanceId -like 'PCI*' -and $_.FriendlyName -match 'Realtek' } |
    ForEach-Object {
        W ("InstanceId : {0}" -f $_.InstanceId)
        W ("  Friendly : {0}" -f $_.FriendlyName)
        W ("  Status   : {0}" -f $_.Status)
        W ("  Problem  : {0}" -f $_.Problem)
        W ("  ConfigErr: {0}" -f $_.ConfigManagerErrorCode)
    }

W "`n--- 3. Matching driver + its INF ---"
Get-CimInstance Win32_PnPSignedDriver | Where-Object { $_.DeviceName -match 'Realtek' } |
    Select-Object DeviceName, DeviceID, DriverVersion, DriverDate, InfName, IsSigned |
    Format-List | Out-String | Tee-Object -Append

W "`n--- 4. Net class instances (ComponentId / NetCfgInstanceId / Driver) ---"
$net = 'HKLM:\SYSTEM\CurrentControlSet\Control\Class\{4D36E972-E325-11CE-BFC1-08002BE10318}'
Get-ChildItem $net | ForEach-Object {
    $p = Get-ItemProperty $_.PSPath
    W ("[{0}] Desc='{1}' Comp='{2}' NetCfgId='{3}' Driver='{4}'" -f `
        $_.PSChildName, $p.DriverDesc, $p.ComponentId, $p.NetCfgInstanceId, $p.Driver)
    $sub = Join-Path $_.PSPath 'Ndi'
    if (Test-Path $sub) {
        $n = Get-ItemProperty $sub
        W ("      Ndi\Service='{0}'" -f $n.Service)
    }
}

W "`n--- 5. CriticalDeviceDatabase rows ---"
$cddb = 'HKLM:\SYSTEM\CurrentControlSet\Control\CriticalDeviceDatabase'
Get-ChildItem $cddb | ForEach-Object {
    $p = Get-ItemProperty $_.PSPath
    W ("{0,-58} Service={1,-14} Flags={2}" -f $_.PSChildName, $p.Service, $p.Flags)
}

W "`n--- 6. Realtek-related services (Start/Type/ImagePath must exist) ---"
foreach ($s in 'Rt640x64','rt640x64_25g','rt68cx64','RTL8023x64','e1d68x64','e2f68','E1G60','e1iexpress','rteth') {
    $k = "HKLM:\SYSTEM\CurrentControlSet\Services\$s"
    if (Test-Path $k) {
        $p = Get-ItemProperty $k
        $ip = $p.ImagePath
        $resolved = $null
        if ($ip) {
            $clean = ($ip -replace '^\\SystemRoot', $env:SystemRoot) -replace '^System32', "$env:SystemRoot\System32"
            $clean = $clean.Trim('"')
            $resolved = if (Test-Path $clean) { 'FILE OK' } else { 'FILE MISSING' }
        }
        W ("{0,-14} Start={1} Type={2} ErrCtrl={3} Path='{4}' {5}" -f $s,$p.Start,$p.Type,$p.ErrorControl,$ip,$resolved)
    }
}

W "`n--- 7. Exact error text from System log (Realtek / boot / 0xC00102) ---"
Get-WinEvent -FilterHashtable @{LogName='System'; StartTime=(Get-Date).AddDays(-7)} -MaxEvents 4000 |
    Where-Object { $_.Message -match 'Realtek|Rtnic|rt640|0xC00102|0xc00102|Boot Bus|network' } |
    Select-Object -First 40 TimeCreated, Id, ProviderName, LevelDisplayName, Message |
    Format-List | Out-String | Tee-Object -Append

W "`n--- 8. SetupAPI device install log for the NIC ---"
Get-PnpDevice -Class Net | Where-Object { $_.FriendlyName -match 'Realtek' } | ForEach-Object {
    $key = "HKLM:\SYSTEM\CurrentControlSet\Enum\$($_.InstanceId)"
    if (Test-Path $key) {
        W ("Enum key present: {0}" -f $_.InstanceId)
        (Get-ItemProperty $key) | Format-List | Out-String | Tee-Object -Append
    }
}

W "`n--- 9. BCD (is the boot path iSCSI?) ---"
try { & bcdedit /enum all 2>&1 | Select-Object -First 60 | Out-String | Tee-Object -Append } catch { W "bcdedit failed" }

"`n=== done -> $out ==="