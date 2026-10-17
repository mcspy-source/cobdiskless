import struct

data = open('full_system.hive', 'rb').read()

def get_cell(offset):
    pos = 0x1000 + offset
    size = struct.unpack('<i', data[pos:pos+4])[0]
    return pos, size, data[pos+4:pos+abs(size)]

def read_nk(offset):
    pos, size, cell_data = get_cell(offset)
    sig = cell_data[:2]
    if sig != b'nk':
        return None
    name_len = struct.unpack('<H', cell_data[0x48:0x4a])[0]
    name = cell_data[0x4c:0x4c+name_len].decode('latin1', errors='ignore')
    subkeys_count = struct.unpack('<I', cell_data[0x14:0x18])[0]
    subkeys_list_off = struct.unpack('<I', cell_data[0x1c:0x20])[0]
    values_count = struct.unpack('<I', cell_data[0x24:0x28])[0]
    values_list_off = struct.unpack('<I', cell_data[0x28:0x2c])[0]
    return {
        'name': name,
        'subkeys_count': subkeys_count,
        'subkeys_list_off': subkeys_list_off,
        'values_count': values_count,
        'values_list_off': values_list_off,
        'cell_pos': pos,
        'offset': offset
    }

def read_vk(offset):
    pos, size, cell_data = get_cell(offset)
    sig = cell_data[:2]
    if sig != b'vk':
        return None
    name_len = struct.unpack('<H', cell_data[0x02:0x04])[0]
    data_len = struct.unpack('<I', cell_data[0x04:0x08])[0]
    data_off = struct.unpack('<I', cell_data[0x08:0x0c])[0]
    val_type = struct.unpack('<I', cell_data[0x0c:0x10])[0]
    name = cell_data[0x14:0x14+name_len].decode('latin1', errors='ignore') if name_len > 0 else '(Default)'
    is_inline = (data_len & 0x80000000) != 0
    raw_val = b''
    if is_inline:
        real_len = data_len & 0x7FFFFFFF
        raw_val = cell_data[0x08:0x08+min(4, real_len)]
    else:
        vpos, vsize, vdata = get_cell(data_off)
        raw_val = vdata[:data_len]
    return {'name': name, 'type': val_type, 'data': raw_val}

def get_subkeys(nk):
    if nk['subkeys_count'] == 0 or nk['subkeys_list_off'] == 0xFFFFFFFF:
        return []
    pos, size, cell_data = get_cell(nk['subkeys_list_off'])
    sig = cell_data[:2]
    offsets = []
    if sig in (b'lf', b'lh'):
        count = struct.unpack('<H', cell_data[2:4])[0]
        for i in range(count):
            off = struct.unpack('<I', cell_data[4 + i*8: 4 + i*8 + 4])[0]
            offsets.append(off)
    elif sig == b'ri':
        count = struct.unpack('<H', cell_data[2:4])[0]
        for i in range(count):
            sub_list_off = struct.unpack('<I', cell_data[4 + i*4: 4 + i*4 + 4])[0]
            spos, ssize, scell = get_cell(sub_list_off)
            ssig = scell[:2]
            scount = struct.unpack('<H', scell[2:4])[0]
            if ssig in (b'lf', b'lh'):
                for j in range(scount):
                    off = struct.unpack('<I', scell[4 + j*8: 4 + j*8 + 4])[0]
                    offsets.append(off)
    res = []
    for off in offsets:
        sub_nk = read_nk(off)
        if sub_nk:
            res.append(sub_nk)
    return res

def get_values(nk):
    if nk['values_count'] == 0 or nk['values_list_off'] == 0xFFFFFFFF:
        return []
    pos, size, cell_data = get_cell(nk['values_list_off'])
    res = []
    for i in range(nk['values_count']):
        vk_off = struct.unpack('<I', cell_data[i*4:(i+1)*4])[0]
        vk = read_vk(vk_off)
        if vk:
            res.append(vk)
    return res

def find_path(path):
    parts = path.strip('\\').split('\\')
    cur = read_nk(0x20)
    for p in parts:
        subs = get_subkeys(cur)
        found = False
        for s in subs:
            if s['name'].lower() == p.lower():
                cur = s
                found = True
                break
        if not found:
            return None
    return cur

for p in [
    'MountedDevices', 
    'ControlSet001\\Control\\Session Manager\\Memory Management', 
    'ControlSet001\\Services\\rt640x64',
    'ControlSet001\\Services\\rtcx21',
    'ControlSet001\\Services\\ms_wfplwf',
    'ControlSet001\\Services\\WFPLWFS',
    'ControlSet001\\Services\\iScsiPrt',
    'ControlSet001\\Control\\ServiceGroupOrder',
    'Select'
]:
    nk = find_path(p)
    if nk:
        sc = nk['subkeys_count']
        vc = nk['values_count']
        print(f"=== Path: {p} (subkeys: {sc}, values: {vc}) ===")
        vals = get_values(nk)
        for v in vals:
            val_repr = v['data']
            if v['type'] == 4 and len(v['data']) == 4:
                val_repr = f"DWORD:{struct.unpack('<I', v['data'])[0]}"
            elif v['type'] in (1, 2):
                val_repr = f"STR:{v['data'].decode('utf-16le', errors='ignore').rstrip(chr(0))}"
            elif v['type'] == 7: # MULTI_SZ
                strings = v['data'].decode('utf-16le', errors='ignore').split('\x00')
                val_repr = f"MULTI_SZ:{strings}"
            else:
                val_repr = f"RAW:{v['data'][:30]}"
            print(f"   {v['name']} (type={v['type']}): {val_repr}")
    else:
        print(f"=== Path: {p} NOT FOUND ===")
