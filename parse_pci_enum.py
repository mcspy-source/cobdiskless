import re

with open('pci_enum.txt', 'r', encoding='utf-16le', errors='ignore') as f:
    text = f.read()

devices = set()
for m in re.finditer(r'Enum\\PCI\\([^\r\n\\]+)\\([^\r\n\\]+)', text):
    if not m.group(2).startswith('Properties'):
        devices.add(f'{m.group(1)} \\ {m.group(2)}')

print(f'Total devices found: {len(devices)}')
for d in sorted(devices):
    print(d)
