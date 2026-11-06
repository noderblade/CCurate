#!/usr/bin/env python3
"""Exercise real decoder scratch buffers, source access and worker diagnostics.

Usage: python3 tests/preview_limits.py build/previewcheck build/libpreview_policy_probe.so
Requires Pillow. The interposer is used only with generated disposable fixtures.
"""
from pathlib import Path
from PIL import Image
import hashlib
import os
import resource
import subprocess as sp
import sys
import tempfile

app, probe = [str(Path(p).resolve()) for p in sys.argv[1:3]]
with tempfile.TemporaryDirectory(prefix='ccurate-preview-limits-') as temp:
    image = Path(temp) / 'photo.png'
    Image.new('RGB', (1280, 800), (31, 156, 87)).save(image)
    before = hashlib.sha512(image.read_bytes()).digest()
    env = os.environ.copy()
    env.update(LD_PRELOAD=probe, CCURATE_TEST_PREVIEW_POLICY='normal')
    result = sp.run([app, '--spawn', image], env=env, capture_output=True, text=True, timeout=25)
    assert result.returncode == 0 and 'pixels=1000x625' in result.stdout, result.stdout + result.stderr
    assert hashlib.sha512(image.read_bytes()).digest() == before
    print('PASS real memfd and temporary-file writes / source read-only / Internet blocked / local IPC')
    env['CCURATE_TEST_PREVIEW_POLICY'] = 'oversize'
    result = sp.run([app, '--spawn', image], env=env, capture_output=True, text=True, timeout=25)
    assert result.returncode == 1 and '512 MiB temporary-file/shared-memory limit' in result.stdout, result.stdout
    assert 'Image preview backend: GdkPixbuf' in result.stdout
    assert hashlib.sha512(image.read_bytes()).digest() == before
    print('PASS oversized scratch buffer has a specific limit diagnostic and preserves input')

    def lower_limit():
        resource.setrlimit(resource.RLIMIT_FSIZE, (1024 * 1024, 1024 * 1024))

    result = sp.run([app, '--spawn', image], capture_output=True, text=True, timeout=25,
                    preexec_fn=lower_limit)
    assert result.returncode == 1 and 'cannot configure worker resource limits:' in result.stdout, result.stdout
    assert hashlib.sha512(image.read_bytes()).digest() == before
    print('PASS inherited hard limit reports setup failure instead of silently ignoring it')

    image.write_bytes(image.read_bytes()[:40])
    result = sp.run([app, '--spawn', image], capture_output=True, text=True, timeout=25)
    assert result.returncode == 1 and 'Image decoder:' in result.stdout, result.stdout
    assert 'Decoder diagnostics: Image preview backend: GdkPixbuf' in result.stdout, result.stdout
    print('PASS decoder-returned errors preserve backend/stderr diagnostics')
