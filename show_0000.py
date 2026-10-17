with open('net_class.txt', 'r', encoding='utf-16le', errors='ignore') as f:
    text = f.read()

lines = text.splitlines()
in_0000 = False
for line in lines:
    if line.strip() == r'HKEY_LOCAL_MACHINE\TEMP_SYS\ControlSet001\Control\Class\{4d36e972-e325-11ce-bfc1-08002be10318}\0000':
        in_0000 = True
    elif line.startswith('HKEY_LOCAL_MACHINE'):
        in_0000 = False
    if in_0000:
        print(line)
