#!/usr/bin/env python3
"""Stage the app-local Lavapipe ICD and native DLLs without registering a driver."""
from pathlib import Path
import os
import re
import shutil
import subprocess
import sys


def stage(destination, prefix=None):
    compiler = shutil.which('gcc')
    assert compiler or prefix, 'Run inside MSYS2 UCRT64'
    prefix = Path(prefix or Path(compiler).resolve().parent.parent)
    assert prefix.name.lower() == 'ucrt64', prefix
    destination = Path(destination).resolve()
    destination.mkdir(parents=True, exist_ok=True)
    available = {p.name.lower(): p for p in (prefix / 'bin').glob('*.dll')}
    # Raven and the external-device owner must use the same modern loader.
    # This also makes the un-packaged build usable for the native GUI tests.
    shutil.copy2(available['vulkan-1.dll'], destination.parent / 'vulkan-1.dll')
    system = Path(os.environ['SystemRoot']) / 'System32'
    queue, copied = ['vulkan_lvp.dll'], set()
    assert queue[0] in available, 'Install mingw-w64-ucrt-x86_64-mesa for CPU rendering'
    while queue:
        name = queue.pop().lower()
        assert name != 'msys-2.0.dll', 'Software rendering must not depend on MSYS2'
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
    # Direct driver loading uses the DLL entry point, not an installed ICD JSON.
    # The main packager includes the prefix's third-party license directory.
    print(f'Staged packaged software Vulkan: {len(copied)} native DLLs in {destination}')
    return copied


if __name__ == '__main__':
    stage(sys.argv[1])
