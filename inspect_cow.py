import sys

with open(r'E:\wb_6c4b902f61eb.cow', 'rb') as f:
    data = f.read()

nonzero = sum(1 for b in data if b != 0)
print(f"Total bytes: {len(data)}, Non-zero bytes: {nonzero}")

# Check first 256 non-zero bytes or offsets
non_zero_offsets = [i for i, b in enumerate(data) if b != 0]
if non_zero_offsets:
    print(f"First non-zero offset: {non_zero_offsets[0]}, Last non-zero offset: {non_zero_offsets[-1]}")
    # print what LBAs or sectors these might be
    # COW header is usually 64 or 512 bytes
    print(f"Sample data at {non_zero_offsets[0]}: {data[non_zero_offsets[0]:non_zero_offsets[0]+32]}")
