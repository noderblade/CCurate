#!/usr/bin/env python3
"""Regression for the actual preview subprocess, including EXIF rotations.
Requires Pillow; all fixtures are generated in a disposable directory.
"""
from pathlib import Path
from PIL import Image
import hashlib
import re
import subprocess as sp
import sys
import tempfile
app = str(Path(sys.argv[1]).resolve())
with tempfile.TemporaryDirectory(prefix='ccurate-preview-process-') as temp:
    root = Path(temp)
    def check(path, expected, ok=True):
        before = hashlib.sha512(path.read_bytes()).digest()
        result = sp.run([app, '--spawn', str(path)], capture_output=True, text=True, timeout=25)
        assert hashlib.sha512(path.read_bytes()).digest() == before, 'Preview modified its input'
        assert (result.returncode == 0) == ok, (path.name, result.stdout, result.stderr)
        assert expected in result.stdout, (path.name, result.stdout)
        dims = re.search(r'pixels=(\d+)x(\d+)', result.stdout)
        w, h = map(int, dims.groups())
        assert 0 <= w <= 1000 and 0 <= h <= 800, (path.name, w, h)
        print(f'PASS subprocess {path.name}: {w}x{h}', flush=True)
        return w, h
    for orient in range(1, 9):
        image = Image.new('RGB', (1280, 800), (38, 135, 202))
        exif = Image.Exif(); exif[274] = orient
        path = root / f'exif-{orient}.jpg'; image.save(path, exif=exif)
        dims = check(path, 'Original image: 1280 x 800')
        assert dims == ((1000, 625) if orient <= 4 else (500, 800)), dims
    for name, size, mode, options in [
        ('portrait', (2000, 5000), 'RGB', {}),
        ('panorama', (12000, 400), 'RGB', {}),
        ('progressive', (2048, 1024), 'RGB', {'progressive': True}),
        ('cmyk', (800, 1280), 'CMYK', {}),
        ('one-pixel-wide', (1, 2000), 'RGB', {}),
    ]:
        path = root / (name + '.jpg'); Image.new(mode, size).save(path, **options)
        check(path, 'Original image:')
    valid = (root / 'exif-1.jpg').read_bytes()
    for fmt in ['PNG', 'GIF', 'BMP', 'TIFF']:
        path = root / ('image.' + fmt.lower())
        Image.new('RGB', (1600, 1200), (200, 75, 35)).save(path, format=fmt)
        assert check(path, 'Original image:') == (1000, 750)
    bad = root / 'corrupt.jpg'; bad.write_bytes(valid[:100])
    check(bad, 'Image decoder:', ok=False)
    txt = root / 'note.txt'; txt.write_text('Preview subprocess plain text fixture\n')
    check(txt, 'Preview subprocess plain text fixture')
    cancelled = sp.run([app, '--cancel', str(txt)], capture_output=True, text=True, timeout=5)
    assert cancelled.returncode == 1 and 'Preview cancelled by a newer selection.' in cancelled.stdout
    print('PASS: preview cancellation terminates and collects the worker')
print('20 preview subprocess cases PASS')
