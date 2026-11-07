#!/usr/bin/env python3
"""Stage only a software Vulkan driver's dependencies for headless Windows CI."""
from pathlib import Path
import json
import os
import re
import shutil
import subprocess
import sys

prefix = Path(shutil.which('gcc')).resolve().parent.parent
destination = Path(sys.argv[1]).resolve()
destination.mkdir(parents=True, exist_ok=True)
available = {p.name.lower(): p for p in (prefix / 'bin').glob('*.dll')}
system = Path(os.environ['SystemRoot']) / 'System32'
queue, copied = ['vulkan_lvp.dll'], set()
while queue:
    name = queue.pop().lower()
    if name in copied:
        continue
    if name not in available:
        assert name.startswith(('api-ms-', 'ext-ms-')) or (system / name).exists(), name
        continue
    source = available[name]
    shutil.copy2(source, destination / source.name)
    copied.add(name)
    imports = subprocess.check_output(['objdump', '-p', str(source)], text=True)
    queue.extend(re.findall(r'DLL Name:\s*(\S+)', imports))
assert 'msys-2.0.dll' not in copied
manifest = json.loads((prefix / 'share/vulkan/icd.d/lvp_icd.x86_64.json').read_text())
manifest['ICD']['library_path'] = str(destination / available['vulkan_lvp.dll'].name)
(destination / 'lvp_icd.json').write_text(json.dumps(manifest), encoding='utf-8')
print(f'Staged software Vulkan and {len(copied) - 1} driver dependencies at {destination}')
