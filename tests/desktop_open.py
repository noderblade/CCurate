#!/usr/bin/env python3
"""Launch the real GIO worker with isolated desktop associations and read-only inputs."""
from pathlib import Path
from desktop_fixture import desktop_fixture
import hashlib
import json
import os
import subprocess as sp
import sys
import tempfile
import time

app = str(Path(sys.argv[1]).resolve())
with tempfile.TemporaryDirectory(prefix='ccurate-open-') as temp:
    root = Path(temp)
    source = root / 'source'; source.mkdir()
    env, record = desktop_fixture(root)
    rootfd = os.open(source, os.O_RDONLY | os.O_DIRECTORY)
    def launch(name, ok):
        read, write = os.pipe()
        try:
            result = sp.run([app, '--open-worker', str(rootfd), str(write), name],
                            env=env, pass_fds=(rootfd,write), timeout=10)
        finally:
            os.close(write)
        message = os.read(read, 8192).decode(); os.close(read)
        assert (result.returncode == 0) == ok, (name, message)
        return message
    names = ['document with spaces.txt', 'quote" dollar$; percent% #.txt', 'nonutf8-\udcff.txt', '-leading.txt', 'document.pdf']
    for count, name in enumerate(names,1):
        path = source / name; path.write_text('%PDF-1.4\n1 0 obj << /Type /Catalog >> endobj\n%%EOF' if name.endswith('.pdf') else 'Do not change this source.\n'); path.chmod(0o444)
        before = hashlib.sha512(path.read_bytes()).digest()
        launch(name, True)
        deadline = time.monotonic()+5
        while not record.exists() or len(record.read_text().splitlines()) < count:
            assert time.monotonic() < deadline, 'Default application did not launch'
            time.sleep(.02)
        last = json.loads(record.read_text().splitlines()[-1])
        assert last == [str(path)], last
        assert hashlib.sha512(path.read_bytes()).digest() == before
    assert 'Cannot open' in launch('missing.txt', False)
    (source / 'escape.txt').symlink_to('/etc/hosts')
    assert 'Cannot open' in launch('escape.txt', False)
    os.mkfifo(source / 'pipe')
    assert 'unsupported file type' in launch('pipe', False)
    assert 'Cannot open' in launch('../recorder.py', False)
    os.close(rootfd)
    print('PASS default desktop association / exact filenames / no shell evaluation / missing file / symlink / FIFO / path escape / unchanged source')
