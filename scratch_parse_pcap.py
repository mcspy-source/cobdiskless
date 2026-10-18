import struct

def parse():
    with open('logs/vm1_net.pcap', 'rb') as f:
        hdr = f.read(24)
        count = 0
        first_ts = None
        while True:
            pkt_hdr = f.read(16)
            if len(pkt_hdr) < 16:
                break
            ts_sec, ts_usec, incl_len, orig_len = struct.unpack('<IIII', pkt_hdr)
            data = f.read(incl_len)
            count += 1
            ts = ts_sec + ts_usec / 1e6
            if first_ts is None: first_ts = ts
            rel_ts = ts - first_ts

            if len(data) >= 14:
                ethertype = data[12:14].hex()
                if ethertype == '0800':
                    proto = data[23]
                    src = '.'.join(str(b) for b in data[26:30])
                    dst = '.'.join(str(b) for b in data[30:34])
                    if proto == 17:
                        sport, dport = struct.unpack('>HH', data[34:38])
                        if count <= 20 or count % 5000 == 0:
                            print(f'Pkt {count} T+{rel_ts:.2f}s: UDP {src}:{sport} -> {dst}:{dport} len={len(data)}')
                    elif proto == 6:
                        sport, dport = struct.unpack('>HH', data[34:38])
                        flags = data[47]
                        flags_list = []
                        if flags & 0x02: flags_list.append('SYN')
                        if flags & 0x10: flags_list.append('ACK')
                        if flags & 0x01: flags_list.append('FIN')
                        if flags & 0x04: flags_list.append('RST')
                        if count <= 25 or flags & 0x06: # SYN or RST
                            print(f'Pkt {count} T+{rel_ts:.2f}s: TCP {src}:{sport} -> {dst}:{dport} [{"|".join(flags_list)}] len={len(data)}')
                elif ethertype == '0806':
                    print(f'Pkt {count} T+{rel_ts:.2f}s: ARP len={len(data)}')

if __name__ == '__main__':
    parse()
