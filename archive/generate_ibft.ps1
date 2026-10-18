param(
    [string]$Mac = "52:54:00:12:34:01",
    [string]$StationId = "TEST-INTEL-01",
    [string]$TargetIqn = "iqn.2026-09.com.cobdiskless:TEST-INTEL-01",
    [string]$InitiatorIqn = "iqn.1991-05.com.microsoft:52-54-00-12-34-01",
    [string]$TargetIp = "192.168.1.99",
    [int]$TargetPort = 3260,
    [string]$NicIp = "192.168.1.101",
    [string]$NicSubnet = "255.255.255.0",
    [string]$NicGateway = "192.168.1.1",
    [int]$PciBus = 0,
    [int]$PciDev = 3,
    [int]$PciFunc = 0,
    [string]$OutFile = "logs\ibft_body.bin"
)

# iBFT binary structure generator
# Header: 48 bytes (ACPI table header)
# Initiator Control Block: Type 1
# NIC Control Block: Type 2
# Target Control Block: Type 3

function Write-Ip6([System.IO.BinaryWriter]$bw, [string]$ipv4Str) {
    # 16 bytes for IPv4-mapped IPv6 or IPv4 in low bytes: 00..00 FFFF IPv4
    $parts = $ipv4Str.Split('.') | ForEach-Object { [byte]$_ }
    $bytes = New-Object byte[] 16
    $bytes[10] = 0xFF
    $bytes[11] = 0xFF
    $bytes[12] = $parts[0]
    $bytes[13] = $parts[1]
    $bytes[14] = $parts[2]
    $bytes[15] = $parts[3]
    $bw.Write($bytes)
}

$ms = New-Object System.IO.MemoryStream
$bw = New-Object System.IO.BinaryWriter($ms)

# We will calculate string offsets from the beginning of iBFT table.
# Fixed table layout:
# Offset 0..47: iBFT Table Header (48 bytes)
# Offset 48..65: Control Structure (18 bytes)
# Offset 66..145: Initiator Structure (74 + InitiatorName len)
# ...

# Let's align on standard iBFT offsets:
# Offset 0x30 (48): Control Structure (18 bytes)
# Offset 0x48 (72): Initiator Structure (0x4A = 74 bytes)
# Offset 0x98 (152): NIC Structure (0x66 = 102 bytes)
# Offset 0x100 (256): Target Structure (0x36 = 54 bytes)
# Strings starting at offset 0x140 (320)

$msData = New-Object System.IO.MemoryStream
$bwData = New-Object System.IO.BinaryWriter($msData)

# 1. Control Structure (Offset 0x30 in full table, offset 0 in our body if body starts at 0x30, OR body is 0..end)
# NOTE: QEMU -acpitable data=<file> injects data AFTER the 36-byte ACPI header!
# ACPI header is: Sig(4) + Length(4) + Rev(1) + Checksum(1) + OEMID(6) + OEMTableID(8) + OEMRev(4) + CreatorID(4) + CreatorRev(4) = 36 bytes.
# For iBFT, ACPI 3.0 specifies:
# Header: 36 or 48 bytes. Standard iBFT header has 12 reserved bytes after 36 bytes ACPI header = 48 bytes total before Control Structure.
# In SeaBIOS / QEMU, -acpitable sig=iBFT generates the 36-byte ACPI header.
# So the data file starts at offset 36!
# Offset 36..47: 12 reserved bytes (0x00)
# Offset 48 (0x30): Control Structure
# Control Structure (18 bytes):
# Type = 1 (1 byte)
# Version = 1 (1 byte)
# Length = 18 (2 bytes)
# Flags = 0 (1 byte)
# Reserved = 0 (1 byte)
# Initiator Offset = 0x48 (2 bytes)
# NIC 0 Offset = 0x98 (2 bytes)
# Target 0 Offset = 0x100 (2 bytes)
# Reserved = 6 bytes

# Let's construct cleanly:
$initOffset = 0x48
$nicOffset  = 0x98
$tgtOffset  = 0x100
$strOffset  = 0x140

# Pad 12 bytes reserved for ACPI header remainder (offset 36 to 48)
$bwData.Write((New-Object byte[] 12))

# Control Structure @ 0x30 (48)
$bwData.Write([byte]1) # Type 1
$bwData.Write([byte]1) # Version 1
$bwData.Write([uint16]18) # Length
$bwData.Write([byte]0) # Flags
$bwData.Write([byte]0) # Reserved
$bwData.Write([uint16]$initOffset)
$bwData.Write([uint16]$nicOffset)
$bwData.Write([uint16]$tgtOffset)
$bwData.Write((New-Object byte[] 6)) # Reserved

# Pad up to $initOffset (0x48 = 72)
while ($msData.Position + 36 -lt $initOffset) {
    $bwData.Write([byte]0)
}

# Initiator Structure @ 0x48 (72)
# Type = 2, Version = 1, Length = 74 bytes (0x4A), Flags = 3 (Block Valid | Boot Selected)
# String heap offset for Initiator Name
$initNameBytes = [System.Text.Encoding]::ASCII.GetBytes($InitiatorIqn)
$initNameOffset = $strOffset
$initNameLen = $initNameBytes.Length

$bwData.Write([byte]2) # Type 2
$bwData.Write([byte]1) # Version 1
$bwData.Write([uint16]74) # Length
$bwData.Write([byte]3) # Flags: 1 (Valid) | 2 (Boot)
$bwData.Write([byte]0) # Reserved
# iSNS, SLP, Primary Radius, Secondary Radius (16 bytes each = 64 bytes total)
$bwData.Write((New-Object byte[] 64))
# Initiator Name Length (2 bytes) & Offset (2 bytes)
$bwData.Write([uint16]$initNameLen)
$bwData.Write([uint16]$initNameOffset)

# Pad up to $nicOffset (0x98 = 152)
while ($msData.Position + 36 -lt $nicOffset) {
    $bwData.Write([byte]0)
}

# NIC Structure @ 0x98 (152)
# Type = 3, Version = 1, Length = 102 (0x66), Flags = 3 (Valid | Boot)
$cleanMac = $Mac -replace '[:-]', ''
$pciBdf = ($PciBus -shl 8) -bor ($PciDev -shl 3) -bor $PciFunc

$bwData.Write([byte]3) # Type 3
$bwData.Write([byte]1) # Version 1
$bwData.Write([uint16]102) # Length
$bwData.Write([byte]3) # Flags
$bwData.Write([byte]0) # Reserved
Write-Ip6 $bwData $NicIp # IP (16 bytes)
$bwData.Write([byte]24) # Subnet mask prefix (24 = 255.255.255.0)
$bwData.Write([byte]0) # Origin
Write-Ip6 $bwData $NicGateway # Gateway (16 bytes)
Write-Ip6 $bwData "192.168.1.1" # Primary DNS (16 bytes)
Write-Ip6 $bwData "0.0.0.0" # Secondary DNS (16 bytes)
Write-Ip6 $bwData "192.168.1.99" # DHCP (16 bytes)
$bwData.Write([uint16]0) # VLAN
for ($i = 0; $i -lt 12; $i += 2) {
    $bwData.Write([Convert]::ToByte($cleanMac.Substring($i, 2), 16))
}
$bwData.Write([uint16]$pciBdf) # PCI BDF (2 bytes)
$bwData.Write([uint16]0) # Host Name Length
$bwData.Write([uint16]0) # Host Name Offset

# Pad up to $tgtOffset (0x100 = 256)
while ($msData.Position + 36 -lt $tgtOffset) {
    $bwData.Write([byte]0)
}

# Target Structure @ 0x100 (256)
# Type = 4, Version = 1, Length = 54 (0x36), Flags = 3 (Valid | Boot)
$tgtNameBytes = [System.Text.Encoding]::ASCII.GetBytes($TargetIqn)
$tgtNameOffset = $initNameOffset + $initNameLen + 1
$tgtNameLen = $tgtNameBytes.Length

$bwData.Write([byte]4) # Type 4
$bwData.Write([byte]1) # Version 1
$bwData.Write([uint16]54) # Length
$bwData.Write([byte]3) # Flags
$bwData.Write([byte]0) # Reserved
Write-Ip6 $bwData $TargetIp # Target IP (16 bytes)
$bwData.Write([uint16]$TargetPort) # Port (3260)
$bwData.Write([uint64]0) # LUN (0)
$bwData.Write([byte]0) # Method
$bwData.Write([byte]0) # Reserved
$bwData.Write([uint16]0) # NIC Assoc
$bwData.Write([uint16]0) # Reserved
$bwData.Write([uint16]$tgtNameLen) # Target Name Length
$bwData.Write([uint16]$tgtNameOffset) # Target Name Offset
$bwData.Write([uint16]0) # CHAP Name Len
$bwData.Write([uint16]0) # CHAP Name Offset
$bwData.Write([uint16]0) # CHAP Secret Len
$bwData.Write([uint16]0) # CHAP Secret Offset
$bwData.Write([uint16]0) # Rev CHAP Name Len
$bwData.Write([uint16]0) # Rev CHAP Name Offset
$bwData.Write([uint16]0) # Rev CHAP Secret Len
$bwData.Write([uint16]0) # Rev CHAP Secret Offset

# Pad up to $strOffset (0x140 = 320)
while ($msData.Position + 36 -lt $strOffset) {
    $bwData.Write([byte]0)
}

# Strings @ $strOffset
$bwData.Write($initNameBytes)
$bwData.Write([byte]0)
$bwData.Write($tgtNameBytes)
$bwData.Write([byte]0)

$finalBytes = $msData.ToArray()
[System.IO.File]::WriteAllBytes($OutFile, $finalBytes)
Write-Host "Generated perfect iBFT body: $OutFile ($($finalBytes.Length) bytes)"
