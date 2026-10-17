import json

with open(r'C:\Users\Admin\.gemini\antigravity\brain\81c2d51f-806a-4232-925f-cf02fdceae3a\.system_generated\logs\transcript.jsonl', 'r', encoding='utf-8') as f:
    for line in f:
        if 'Start-Process' in line or 'RunAs' in line or 'arm_pc04' in line:
            obj = json.loads(line)
            tc = obj.get('tool_calls', [])
            for call in tc:
                cmd = call.get('args', {}).get('CommandLine')
                if cmd:
                    print("CMD:", cmd)
