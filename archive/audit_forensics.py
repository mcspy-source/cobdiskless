import subprocess
import os
import sys

def main():
    print("=== FORENSIC AUDIT ===")
    # 0. Mount VHD
    print("Mounting G:\\G\\csmw10.VHD...")
    subprocess.run(['powershell', '-NoProfile', '-Command', 'Mount-DiskImage -ImagePath G:\\G\\csmw10.VHD -StorageType VHD -Access ReadOnly'], check=False)
    
    # 1. Get Mounted volumes
    out = subprocess.check_output(['powershell', '-NoProfile', '-Command', '$img = Get-DiskImage -ImagePath G:\\G\\csmw10.VHD; Get-Disk -Number $img.Number | Get-Partition | Format-Table -AutoSize | Out-String']).decode()
    print("Partitions:\n" + out)

    # Find the drive letter for Partition 2
    drive_out = subprocess.check_output(['powershell', '-NoProfile', '-Command', '$img = Get-DiskImage -ImagePath G:\\G\\csmw10.VHD; (Get-Disk -Number $img.Number | Get-Partition | Where-Object PartitionNumber -eq 2).DriveLetter']).decode().strip()
    print(f"Drive Letter for Partition 2: '{drive_out}'")
    drive = f"{drive_out}:"

    # Query cluster 279582 using fsutil volume querycluster
    print(f"\n--- Querying cluster 279582 on {drive} ---")
    res = subprocess.run(['fsutil', 'volume', 'querycluster', drive, '279582'], capture_output=True, text=True)
    print("STDOUT:\n" + res.stdout[:2000])
    if len(res.stdout) > 2000:
        print(f"... ({len(res.stdout)} bytes total) ...")
        print("LAST 1000 BYTES:\n" + res.stdout[-1000:])
    print("STDERR:\n" + res.stderr)

    # Also let's check cluster 279582 via fsutil file layout on candidate files or scanning MFT
    # Let's inspect the file C:\Windows\system32\en-US\kernelbase.dll.mui on that drive
    cand_path = os.path.join(drive + "\\", "Windows", "System32", "en-US", "kernelbase.dll.mui")
    print(f"\n--- Checking candidate file: {cand_path} ---")
    if os.path.exists(cand_path):
        size = os.path.getsize(cand_path)
        print(f"File exists! Size: {size} bytes")
        layout_res = subprocess.run(['fsutil', 'file', 'layout', cand_path], capture_output=True, text=True)
        print("File Layout:\n" + layout_res.stdout[:3000])
    else:
        print("Candidate file does not exist directly at that path, searching System32...")

    # Let's also check kernelbase.dll
    kb_path = os.path.join(drive + "\\", "Windows", "System32", "KernelBase.dll")
    if os.path.exists(kb_path):
        print(f"\n--- Checking KernelBase.dll: {kb_path} ---")
        print(f"Size: {os.path.getsize(kb_path)} bytes")
        kb_layout = subprocess.run(['fsutil', 'file', 'layout', kb_path], capture_output=True, text=True)
        # Search for 279582 or hex in layout
        for line in kb_layout.stdout.splitlines():
            if '279582' in line or '0x4441e' in line.lower() or 'VCN' in line or 'LCN' in line:
                print("  " + line)

if __name__ == '__main__':
    main()
