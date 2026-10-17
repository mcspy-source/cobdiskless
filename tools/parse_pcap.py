import struct, sys

def analyze(pcap_path):
    with open(pcap_path, 'rb') as f:
        ghdr = f.read(24)
        if len(ghdr) < 24: return
        magic = struct.unpack('<I', ghdr[:4])[0]
        endian = '<' if magic == 0xa1b2c3d4 else '>'
        
        pkt_count = 0
        src_mac_counts = {}
        ip_counts = {}
        tcp_syn_srcs = []
        arps = []
        
        while True:
            phdr = f.read(16)
            if len(phdr) < 16: break
            ts_sec, ts_usec, incl_len, orig_len = struct.unpack(endian + 'IIII', phdr)
            data = f.read(incl_len)
            pkt_count += 1
            if len(data) < 14: continue
            
            src_mac = ':'.join(f'{b:02x}' for b in data[6:12])
            dst_mac = ':'.join(f'{b:02x}' for b in data[0:6])
            eth_type = struct.unpack('>H', data[12:14])[0]
            
            src_mac_counts[src_mac] = src_mac_counts.get(src_mac, 0) + 1
            
            if eth_type == 0x0806: # ARP
                arp_op = struct.unpack('>H', data[20:22])[0]
                src_ip = '.'.join(str(b) for b in data[28:32])
                dst_ip = '.'.join(str(b) for b in data[38:42])
                if pkt_count > 91000 or (ts_sec > 0 and len(arps) < 20):
                    arps.append((ts_sec, arp_op, src_ip, dst_ip, src_mac))
            elif eth_type == 0x0800: # IPv4
                proto = data[23]
                src_ip = '.'.join(str(b) for b in data[26:30])
                dst_ip = '.'.join(str(b) for b in data[30:34])
                ip_counts[src_ip] = ip_counts.get(src_ip, 0) + 1
                if proto == 6 and len(data) >= 34 + 20: # TCP
                    flags = data[47]
                    if flags & 0x02: # SYN
                        tcp_syn_srcs.append((ts_sec, src_ip, dst_ip, flags))

        print(f"Total packets: {pkt_count}")
        print("Source MAC counts:", src_mac_counts)
        print("Source IP counts:", ip_counts)
        print(f"TCP SYNs ({len(tcp_syn_srcs)}):", tcp_syn_srcs[:10])
        print("Last 10 ARPs:", arps[-10:] if arps else [])

if __name__ == '__main__':
    analyze(sys.argv[1] if len(sys.argv) > 1 else 'logs/vm1_net.pcap')
