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

# iBFT body generator -- byte-exact replica of the layout iPXE itself builds
# (cobpxe\src\include\ipxe\ibft.h + drivers\block\ibft.c, ibft_install()).
#
# QEMU -acpitable data=<file> injects this body AFTER a 36-byte ACPI header,
# so every offset written below is (table offset - 36).
#
# Ground truth layout (pairs = 2 fixed at compile time, one NIC/target used):
#   table 0x00..0x23  ACPI header (QEMU-generated)
#   table 0x24..0x2F  12 reserved bytes (body offset 0x00)
#   table 0x30        Control structure, Length=18
#                     id=1 ver=1 len=18 index=0 flags=0 extensions=0
#                     initiator=0x50 nic=0xA0 target=0x180 pair[1]={0,0}
#   table 0x50        Initiator, Length=74  (id=2 ver=1 index=0 flags=3)
#   table 0xA0        NIC,      Length=102 (id=3 ver=1 index=0 flags=3)
#                     (unused pair[1].nic slot at 0x110 stays zero -> offset 0)
#   table 0x180       Target,   Length=54  (id=4 ver=1 index=0 flags=3)
#                     (unused pair[1].target slot at 0x1C0 stays zero -> offset 0)
#   table 0x200       Strings: initiatorIQN NUL targetIQN NUL
#
# CRITICAL (root cause of the 0x7B runs): the structure header is
#   id(1) ver(1) len(2) index(1) flags(1)   <- INDEX at byte 4, FLAGS at byte 5
# and the control block contains a u16 "extensions" field before the offsets.
# A layout missing either makes msiscsi read target-offset=0 (no boot target)
# and flags=0 (BLOCK_VALID clear) -> no session ever gets logged in.
#
# Structures are 16-byte aligned (IBFT_ALIGN 16). All multi-byte fields LE.

function Write-Ip6([System.IO.BinaryWriter]$bw, [string]$ipv4Str) {
    # ibft_ipaddr: zeroes[5] u16, ones u16 (=0xFFFF only when an IPv4 is
    # present), in_addr.  0.0.0.0 / empty => all 16 bytes zero (absent).
    if ([string]::IsNullOrWhiteSpace($ipv4Str) -or $ipv4Str -eq '0.0.0.0') {
        $bw.Write((New-Object byte[] 16))
        return
    }
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

function Get-PrefixLen([string]$mask) {
    # Count leading one-bits octet by octet (no 32-bit arithmetic, so no
    # signed-overflow surprises): 255.255.255.0 -> 24.
    $n = 0
    foreach ($octet in $mask.Split('.')) {
        $b = [int]$octet
        for ($bit = 7; $bit -ge 0; $bit--) {
            if ($b -band ([int]1 -shl $bit)) { $n++ } else { return $n }
        }
    }
    return $n
}

function Pad-To([System.IO.BinaryWriter]$bw, [int]$bodyPos, [int]$tableOffset) {
    # Body position for a given table offset (body starts at table offset 36).
    $target = $tableOffset - 36
    if ($bodyPos -gt $target) {
        throw "layout overrun: body at $bodyPos already past table 0x$($tableOffset.ToString('X'))"
    }
    while ($bw.BaseStream.Position -lt $target) { $bw.Write([byte]0) }
}

$ms = New-Object System.IO.MemoryStream
$bw = New-Object System.IO.BinaryWriter($ms)

$initOffset = 0x50
$nicOffset  = 0xA0
$tgtOffset  = 0x180
$strOffset  = 0x200

$initNameBytes = [System.Text.Encoding]::ASCII.GetBytes($InitiatorIqn)
$tgtNameBytes  = [System.Text.Encoding]::ASCII.GetBytes($TargetIqn)
$initNameOffset = $strOffset
$tgtNameOffset  = $strOffset + $initNameBytes.Length + 1

# --- table 0x24..0x2F: 12 reserved bytes (body offset 0) ---
$bw.Write((New-Object byte[] 12))

# --- table 0x30: Control structure (18 bytes) ---
$bw.Write([byte]1)              # structure_id = CONTROL
$bw.Write([byte]1)              # version = 1
$bw.Write([uint16]18)           # length (header + ext + initiator + pair[2])
$bw.Write([byte]0)              # index
$bw.Write([byte]0)              # flags (SINGLE_LOGIN_ONLY not set)
$bw.Write([uint16]0)            # extensions = 0
$bw.Write([uint16]$initOffset)  # initiator offset
$bw.Write([uint16]$nicOffset)   # pair[0].nic
$bw.Write([uint16]$tgtOffset)   # pair[0].target
$bw.Write([uint16]0)            # pair[1].nic (unused -> 0 = absent)
$bw.Write([uint16]0)            # pair[1].target (unused -> 0 = absent)

# --- table 0x50: Initiator structure (74 bytes) ---
Pad-To $bw ([int]$ms.Position) $initOffset
$bw.Write([byte]2)              # structure_id = INITIATOR
$bw.Write([byte]1)              # version
$bw.Write([uint16]74)           # length
$bw.Write([byte]0)              # index
$bw.Write([byte]3)              # flags: BLOCK_VALID | FIRMWARE_BOOT_SELECTED
$bw.Write((New-Object byte[] 64))   # iSNS + SLP + radius[2] (absent)
$bw.Write([uint16]$initNameBytes.Length)  # initiator name len (excl. NUL)
$bw.Write([uint16]$initNameOffset)        # initiator name offset (table-rel.)

# --- table 0xA0: NIC structure (102 bytes) ---
Pad-To $bw ([int]$ms.Position) $nicOffset
$cleanMac = $Mac -replace '[:-]', ''
$pciBdf = ($PciBus -shl 8) -bor ($PciDev -shl 3) -bor $PciFunc
$bw.Write([byte]3)              # structure_id = NIC
$bw.Write([byte]1)              # version
$bw.Write([uint16]102)          # length
$bw.Write([byte]0)              # index (first NIC)
$bw.Write([byte]3)              # flags: BLOCK_VALID | FIRMWARE_BOOT_SELECTED
Write-Ip6 $bw $NicIp            # IP address
$bw.Write([byte](Get-PrefixLen $NicSubnet))  # subnet mask prefix (bits)
$bw.Write([byte]1)              # origin = MANUAL (static station config)
Write-Ip6 $bw $NicGateway       # default gateway
Write-Ip6 $bw "192.168.1.1"     # primary DNS
Write-Ip6 $bw "0.0.0.0"         # secondary DNS (absent)
Write-Ip6 $bw $TargetIp         # DHCP server (our daemon serves DHCP+target)
$bw.Write([uint16]0)            # VLAN (none)
for ($i = 0; $i -lt 12; $i += 2) {
    $bw.Write([Convert]::ToByte($cleanMac.Substring($i, 2), 16))
}
$bw.Write([uint16]$pciBdf)      # PCI bus:dev:fn (LE), matches -device addr
$bw.Write([uint16]0)            # hostname len (absent)
$bw.Write([uint16]0)            # hostname offset

# --- table 0x180: Target structure (54 bytes) ---
Pad-To $bw ([int]$ms.Position) $tgtOffset
$bw.Write([byte]4)              # structure_id = TARGET
$bw.Write([byte]1)              # version
$bw.Write([uint16]54)           # length
$bw.Write([byte]0)              # index (first target)
$bw.Write([byte]3)              # flags: BLOCK_VALID | FIRMWARE_BOOT_SELECTED
Write-Ip6 $bw $TargetIp         # target IP
$bw.Write([uint16]$TargetPort)  # TCP port (3260)
$bw.Write([uint64]0)            # boot LUN 0
$bw.Write([byte]0)              # chap_type = NONE
$bw.Write([byte]0)              # nic_association = 0 (first NIC)
$bw.Write([uint16]$tgtNameBytes.Length)   # target name len (excl. NUL)
$bw.Write([uint16]$tgtNameOffset)         # target name offset (table-rel.)
$bw.Write([uint16]0)            # chap_name len/absent
$bw.Write([uint16]0)
$bw.Write([uint16]0)            # chap_secret len/absent
$bw.Write([uint16]0)
$bw.Write([uint16]0)            # reverse chap_name len/absent
$bw.Write([uint16]0)
$bw.Write([uint16]0)            # reverse chap_secret len/absent
$bw.Write([uint16]0)

# --- table 0x200: string heap ---
Pad-To $bw ([int]$ms.Position) $strOffset
$bw.Write($initNameBytes)
$bw.Write([byte]0)
$bw.Write($tgtNameBytes)
$bw.Write([byte]0)

$finalBytes = $ms.ToArray()
[System.IO.File]::WriteAllBytes($OutFile, $finalBytes)
Write-Host "Generated iPXE-exact iBFT body: $OutFile ($($finalBytes.Length) bytes)"
exit 0
