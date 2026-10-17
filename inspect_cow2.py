import struct

with open(r'E:\wb_6c4b902f61eb.cow', 'rb') as f:
    data = f.read()

print("Header (hex):", data[:64].hex())
magic, version, block_size = struct.unpack('<4sII', data[:12])
print(f"Magic: {magic}, Version: {version}, Block size: {block_size}")

# Find any strings or text in the COW file:
for i in range(0, len(data) - 4):
    chunk = data[i:i+64]
    # Check if printable ASCII
    printable = "".join(chr(c) if 32 <= c <= 126 else "." for c in chunk)
    if "BCD" in printable or "Windows" in printable or "dump" in printable or "vol" in printable or "System" in printable:
        print(f"Offset {i}: {printable}")
