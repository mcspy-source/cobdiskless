with open('vhd_0000.txt', 'r', encoding='ascii', errors='replace') as f:
    text = f.read()

print('SkipPhyReset in 0000?', 'SkipPhyReset' in text)
print('WaitAutoNegComplete in 0000?', 'WaitAutoNegComplete' in text)
print('PnPCapabilities in 0000?', 'PnPCapabilities' in text)
