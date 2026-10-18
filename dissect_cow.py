import struct

with open(r'E:\wb_6c4b902f61eb.cow', 'rb') as f:
    hdr_data = f.read(4096)
    table_data = f.read(61440)
    data = f.read()

# CowFileHeader:
# char magic[8]; uint32_t version; uint32_t chunk_size; uint64_t capacity_bytes;
# uint32_t entry_count; uint32_t entry_capacity; uint64_t next_free_cluster; uint64_t table_checksum;
magic, version, chunk_size, capacity_bytes, entry_count, entry_capacity, next_free_cluster, table_checksum = struct.unpack(
    '<8sIIQIIQQ', hdr_data[:48]
)

print(f"Magic: {magic}")
print(f"Version: {version}")
print(f"Chunk size: {chunk_size}")
print(f"Capacity bytes: {capacity_bytes} ({capacity_bytes / (1024**3):.2f} GB)")
print(f"Entry count: {entry_count}")
print(f"Entry capacity: {entry_capacity}")
print(f"Next free cluster: {next_free_cluster}")

print("\n--- Allocated Table Entries ---")
entries = []
for i in range(entry_count):
    v_chunk, f_cluster = struct.unpack('<II', table_data[i*8:(i+1)*8])
    entries.append((v_chunk, f_cluster))
    # virtual LBA range:
    # virtual_chunk * (65536 / 512)
    start_lba = v_chunk * (chunk_size // 512)
    end_lba = start_lba + (chunk_size // 512) - 1
    file_offset = 65536 + f_cluster * chunk_size
    print(f"Entry {i}: virtual_chunk={v_chunk} (LBAs {start_lba} .. {end_lba}) -> file_cluster={f_cluster} (file offset {file_offset})")

# Let's inspect the data written in each cluster
for i, (v_chunk, f_cluster) in enumerate(entries):
    c_data = data[f_cluster * chunk_size : (f_cluster + 1) * chunk_size]
    print(f"\n--- Cluster {f_cluster} Content Summary (virtual chunk {v_chunk}) ---")
    nz = sum(1 for b in c_data if b != 0)
    print(f"Non-zero bytes: {nz} / {len(c_data)}")
    
    # Print out strings / sectors
    for sec in range(chunk_size // 512):
        s_data = c_data[sec*512:(sec+1)*512]
        if any(b != 0 for b in s_data):
            lba = v_chunk * (chunk_size // 512) + sec
            preview = "".join(chr(b) if 32 <= b <= 126 else "." for b in s_data[:64])
            print(f"  Sec {sec:3d} (LBA {lba:8d}): {preview}")
