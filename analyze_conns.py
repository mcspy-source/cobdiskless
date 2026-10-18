import struct
import os

MEM_HI_PATH = r"logs\vm1_mem2_hi.bin"

def analyze_mem_hi():
    if not os.path.exists(MEM_HI_PATH):
        print(f"File not found: {MEM_HI_PATH}")
        return

    print(f"Reading {MEM_HI_PATH}...")
    with open(MEM_HI_PATH, "rb") as f:
        # Scan for connection object signature 0x05D8ACAC
        # Search in 4MB chunks
        chunk_size = 32 * 1024 * 1024
        offset = 0
        found_conns = []
        
        while True:
            chunk = f.read(chunk_size)
            if not chunk:
                break
            
            p = 0
            while True:
                idx = chunk.find(b"\xac\xac\xd8\x05", p)
                if idx == -1:
                    break
                abs_offset = offset + idx
                # Read candidate object fields
                if idx + 0x40 <= len(chunk):
                    state = struct.unpack_from("<I", chunk, idx + 0xc)[0]
                    found_conns.append((abs_offset, state, chunk[idx:idx+0x40]))
                p = idx + 4
            offset += len(chunk)

    print(f"Found {len(found_conns)} connection object candidates:")
    for off, state, raw in found_conns:
        print(f"  Offset: 0x{off:x} (Phys: 0x{4*1024*1024*1024 + off:x}), state [conn+0xc]: {state}")
        print("  Raw hex:", raw[:32].hex())

if __name__ == "__main__":
    analyze_mem_hi()
