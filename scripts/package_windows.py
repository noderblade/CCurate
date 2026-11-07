#!/usr/bin/env python3
"""Stage a native UCRT64 application and its recursively discovered DLLs."""
from pathlib import Path
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys

exe, destination = (Path(value).resolve() for value in sys.argv[1:3])
compiler = shutil.which('gcc')
assert compiler, 'Run packaging inside MSYS2 UCRT64'
prefix = Path(compiler).resolve().parent.parent
assert prefix.name.lower() == 'ucrt64', prefix
destination.mkdir(parents=True, exist_ok=True)
shutil.copy2(exe, destination / 'ccurate.exe')
bin_dir = prefix / 'bin'
available = {p.name.lower(): p for p in bin_dir.glob('*.dll')}
system = Path(os.environ['SystemRoot']) / 'System32'
queue = [destination / 'ccurate.exe']
for name in ['vulkan-1.dll']:
    if name in available:
        shutil.copy2(available[name], destination / available[name].name)
        queue.append(destination / available[name].name)

resources = ['share/misc/magic.mgc', 'etc/fonts', 'share/poppler',
             'lib/gdk-pixbuf-2.0/2.10.0/loaders']
for relative in resources:
    source, target = prefix / relative, destination / relative
    if not source.exists():
        if relative == 'share/poppler':
            continue
        raise FileNotFoundError(source)
    target.parent.mkdir(parents=True, exist_ok=True)
    if source.is_dir():
        shutil.copytree(source, target, dirs_exist_ok=True)
        queue.extend(target.rglob('*.dll'))
    else:
        shutil.copy2(source, target)

checked = set()
while queue:
    binary = queue.pop()
    if binary.name.lower() in checked:
        continue
    checked.add(binary.name.lower())
    result = subprocess.check_output(['objdump', '-p', str(binary)], text=True, errors='replace')
    for name in re.findall(r'DLL Name:\s*(\S+)', result):
        key = name.lower()
        assert key != 'msys-2.0.dll', 'The application must not depend on an MSYS runtime'
        if key in checked:
            continue
        if key in available:
            target = destination / available[key].name
            if not target.exists():
                shutil.copy2(available[key], target)
            queue.append(target)
        elif key.startswith(('api-ms-', 'ext-ms-')) or (system / name).exists():
            continue
        else:
            raise RuntimeError(f'Missing DLL: {name}, required by {binary.name}')

# Workers run with their working directory set to the application directory.
# The cache uses relative DLL paths, not the CI machine's installation prefix.
loaders = destination / 'lib/gdk-pixbuf-2.0/2.10.0/loaders'
loader_files = list(loaders.glob('*.dll'))
cache = subprocess.check_output([str(bin_dir / 'gdk-pixbuf-query-loaders.exe'),
                                 *[str(p) for p in loader_files]], text=True, encoding='utf-8')
cache = '\n'.join(line for line in cache.splitlines() if not line.startswith('#')) + '\n'
for loader in loader_files:
    relative = loader.relative_to(destination).as_posix()
    for original in [str(loader).replace('\\', '\\\\'), str(loader).replace('\\', '/')]:
        cache = cache.replace(original, relative)
assert str(destination.drive).lower() + '/' not in cache.lower().replace('\\', '/'), 'Non-portable loader cache'
(loaders.parent / 'loaders.cache').write_text(cache, encoding='utf-8')

for name in ['README.md', 'USER_GUIDE.md', 'FORMAT.md', 'THIRD_PARTY.md', 'CHANGELOG.md',
             'ARCHITECTURE.md', 'TESTING.md']:
    shutil.copy2(name, destination / name)
shutil.copytree('docs', destination / 'docs', dirs_exist_ok=True)
(destination / 'BUILD_ENVIRONMENT.txt').write_text(
    subprocess.check_output(['pacman', '-Q'], text=True, encoding='utf-8'), encoding='utf-8')
license_dir = destination / 'licenses'
shutil.copytree(prefix / 'share/licenses', license_dir, dirs_exist_ok=True)
for name in ['LICENSE', 'LICENSE.md', 'LICENSE.txt']:
    source = Path('vendor/Raven') / name
    if source.exists():
        shutil.copy2(source, license_dir / ('Raven-' + name))
records = []
for path in sorted(destination.rglob('*')):
    if path.is_file():
        assert path.name.lower() != 'agents.md'
        records.append({'path': path.relative_to(destination).as_posix(),
                        'sha256': hashlib.sha256(path.read_bytes()).hexdigest()})
(destination / 'FILES.json').write_text(json.dumps(records, indent=2) + '\n', encoding='utf-8')
print(f'Packaged {len(records)} files; {len(checked)} native binaries inspected.')
