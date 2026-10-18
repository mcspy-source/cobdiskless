with open(r'src\CobDisklessCore\src\image\reg_bootstart.cpp', 'r', encoding='utf-8') as f:
    lines = f.readlines()

for i, line in enumerate(lines):
    if 'pre_arm_known_nic_drivers' in line or 'apply_iscsi_initiator_contract_live' in line:
        print(f'{i+1}: {line.strip()}')
