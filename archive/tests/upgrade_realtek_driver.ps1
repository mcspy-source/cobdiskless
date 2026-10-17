$vhdPath = "G:\csmw10.VHD"
$driverStoreSource = "C:\Windows\System32\DriverStore\FileRepository\rt640x64.inf_amd64_82d53ae24f2639b8"
$out = @()
$out += "=== Step 1: Mounting $vhdPath ==="

$m = Mount-DiskImage -ImagePath $vhdPath -PassThru
$disk = $m | Get-Disk
$parts = $disk | Get-Partition
$osPart = $parts | Where-Object { $_.PartitionNumber -eq 2 }
if (-not $osPart.DriveLetter) {
    Set-Partition -DiskNumber $disk.Number -PartitionNumber 2 -NewDriveLetter Z
    $letter = "Z"
} else {
    $letter = $osPart.DriveLetter
}
$out += "OS Drive Letter is: ${letter}:"

# Step 2: Check current driver version
$targetDrv = "${letter}:\Windows\System32\drivers\rt640x64.sys"
if (Test-Path $targetDrv) {
    $oldVer = (Get-Item $targetDrv).VersionInfo
    $out += "=== Step 2: Current rt640x64.sys ==="
    $out += "  FileVersion: $($oldVer.FileVersion)"
    $out += "  ProductVersion: $($oldVer.ProductVersion)"
    $out += "  File Size: $((Get-Item $targetDrv).Length) bytes"
} else {
    $out += "=== Step 2: rt640x64.sys NOT found at $targetDrv ==="
}

# Step 3: Replace rt640x64.sys with modern Realtek driver
$newDrv = "$driverStoreSource\rt640x64.sys"
$out += "=== Step 3: Replacing rt640x64.sys from $newDrv ==="
$newVer = (Get-Item $newDrv).VersionInfo
$out += "  New FileVersion: $($newVer.FileVersion)"
Copy-Item -Path $newDrv -Destination $targetDrv -Force
$replacedVer = (Get-Item $targetDrv).VersionInfo
$out += "  Replaced FileVersion: $($replacedVer.FileVersion) ($((Get-Item $targetDrv).Length) bytes)"

# Step 4: Run DISM /Add-Driver
$out += "=== Step 4: Running DISM /Add-Driver ==="
$dismArgs = "/Image:${letter}:\ /Add-Driver /Driver:`"$driverStoreSource`" /Recurse"
$out += "Executing: dism.exe $dismArgs"
$dismOutput = & dism.exe /Image:${letter}:\ /Add-Driver /Driver:"$driverStoreSource" /Recurse 2>&1
$out += $dismOutput

# Step 5: Dismount VHD
$out += "=== Step 5: Dismounting $vhdPath ==="
Dismount-DiskImage -ImagePath $vhdPath
$out += "Dismount complete."

$out | Out-File -FilePath "C:\Users\Admin\Desktop\cobdiskless\upgrade_realtek_result.txt" -Encoding utf8
