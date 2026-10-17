#!/usr/bin/env python3
"""
CobDiskless - COW Write-Back Forensics & String Carver Tool
Parses the UnifiedCowDevice v2 .cow file format and carves diagnostic strings
(setupapi.dev.log, PnP, NDIS, kernel errors, BugChecks) from modified disk clusters.
"""

import sys
import os
import struct
import re

KEYWORDS = [
    "CM_PROB",
    "FAILED_ADD",
    "E1G60",
    "ndis",
    "NDIS",
    "AddDevice",
    "NetworkSetup2",
    "0xC00000",
    "0xc00000",
    "BugCheck",
    "CRITICAL_PROCESS_DIED",
    "INACCESSIBLE_BOOT_DEVICE",
    "STATUS_",
    "SetupAPI",
    "Device Install",
    "StartDevice",
    "DriverEntry",
]

def clean_text(raw_bytes):
    """Attempt decoding as UTF-16LE first, then ASCII/latin1."""
    # Check if mostly UTF-16LE (alternating nulls)
    try:
        # Filter printable
        s = raw_bytes.decode('utf-16le', errors='replace')
        # Check printable ratio
        printable = sum(1 for c in s if c.isprintable() or c in '\r\n\t')
        if printable / max(1, len(s)) > 0.6:
            return s
    except Exception:
        pass
    
    # Fallback to ascii/latin1
    s = raw_bytes.decode('latin1', errors='replace')
    return "".join(c if (c.isprintable() or c in '\r\n\t') else '.' for c in s)

def carve_cow(cow_path, output_limit=100):
    if not os.path.exists(cow_path):
        print(f"[!] COW file not found: {cow_path}")
        return

    file_size = os.path.getsize(cow_path)
    print("=" * 80)
    print("COBDISKLESS WRITE-BACK COW FORENSIC SCANNER")
    print("=" * 80)
    print(f"[*] Target COW Path:   {cow_path}")
    print(f"[*] Physical Size:     {file_size:,} bytes ({file_size / (1024*1024):.2f} MB)")
    
    if file_size < 4096:
        print("[!] File too small for COW header (<4096 bytes)")
        return

    with open(cow_path, 'rb') as f:
        hdr_raw = f.read(4096)
        magic = hdr_raw[:8]
        if not magic.startswith(b'COW2'):
            print(f"[!] Invalid COW magic: {magic} (expected COW2)")
            return

        ver, chunk_sz, cap_bytes, count, cap, next_clus, csum = struct.unpack('<IIQIIQQ', hdr_raw[8:48])
        print(f"[+] Header Magic:      {magic.decode('latin1', errors='ignore').strip()}")
        print(f"[+] COW Version:       {ver}")
        print(f"[+] Cluster Size:      {chunk_sz:,} bytes ({chunk_sz // 1024} KiB)")
        print(f"[+] Allocated Entries: {count:,} / {cap:,} (max {cap * chunk_sz // (1024*1024)} MiB)")
        print(f"[+] Next Free Cluster: {next_clus}")
        print(f"[+] Checksum:          0x{csum:016X}")
        print("-" * 80)

        if count == 0:
            print("[*] COW table contains 0 allocated clusters.")
            return

        # Read table entries (each 8 bytes: virtual_chunk (u32), file_cluster (u32))
        f.seek(4096)
        table_raw = f.read(count * 8)
        entries = []
        for i in range(count):
            vc, fc = struct.unpack('<II', table_raw[i*8:(i+1)*8])
            entries.append((vc, fc))

        print(f"[+] Loaded {len(entries)} cluster mapping entries.")
        
        # Sort by file cluster or virtual chunk
        entries_by_lba = sorted(entries, key=lambda x: x[0])
        print(f"[+] Virtual LBA Range: 0x{entries_by_lba[0][0]*128:08X} ({entries_by_lba[0][0]*128:,}) .. "
              f"0x{entries_by_lba[-1][0]*128 + 127:08X} ({(entries_by_lba[-1][0]+1)*128 - 1:,})")
        print("-" * 80)

        # Build byte patterns for ASCII and UTF-16LE
        patterns = []
        for kw in KEYWORDS:
            patterns.append((kw, "ASCII", kw.encode('ascii')))
            patterns.append((kw, "UTF16", kw.encode('utf-16le')))

        hits_found = 0
        
        for entry_idx, (vc, fc) in enumerate(entries):
            clus_offset = 65536 + fc * chunk_sz
            if clus_offset + chunk_sz > file_size:
                # Truncated or unflushed cluster
                continue
            
            f.seek(clus_offset)
            cluster_data = f.read(chunk_sz)
            virt_lba_start = vc * 128

            # Scan for patterns
            cluster_hits = []
            for kw, encoding, pat in patterns:
                pos = 0
                while True:
                    idx = cluster_data.find(pat, pos)
                    if idx == -1:
                        break
                    cluster_hits.append((idx, kw, encoding))
                    pos = idx + len(pat)

            if not cluster_hits:
                continue

            # Group hits in this cluster to avoid duplicate spam
            # Sort by offset
            cluster_hits.sort(key=lambda x: x[0])
            for hit_offset, kw, encoding in cluster_hits:
                hits_found += 1
                if hits_found > output_limit:
                    print(f"\n[!] Output limit ({output_limit}) reached, stopping scan.")
                    return

                # Calculate relative sector within cluster
                rel_sec = hit_offset // 512
                sec_lba = virt_lba_start + rel_sec
                
                # Context extraction: 350 bytes before and after
                ctx_start = max(0, hit_offset - 350)
                ctx_end = min(len(cluster_data), hit_offset + 350)
                ctx_raw = cluster_data[ctx_start:ctx_end]
                
                cleaned = clean_text(ctx_raw)
                # Split lines and filter
                lines = [l.strip() for l in cleaned.splitlines() if l.strip()]

                print(f"\n>>> [HIT #{hits_found}] Keyword: '{kw}' ({encoding})")
                print(f"    Cluster Index:     {fc} (virt_chunk {vc})")
                print(f"    File Offset:       0x{clus_offset + hit_offset:08X} ({clus_offset + hit_offset:,})")
                print(f"    Virtual LBA:       0x{sec_lba:08X} ({sec_lba:,})")
                print(f"    Context Lines:")
                for line in lines[-8:]:
                    # Truncate line length
                    if len(line) > 120:
                        line = line[:117] + "..."
                    print(f"      | {line}")

        print("\n" + "=" * 80)
        print(f"SCAN COMPLETE: Total Hits Found = {hits_found}")
        print("=" * 80)

if __name__ == '__main__':
    target = r'build\bin\writeback\wb_525400123401.cow'
    if len(sys.argv) > 1:
        target = sys.argv[1]
    limit = 150
    if len(sys.argv) > 2:
        limit = int(sys.argv[2])
    carve_cow(target, limit)
