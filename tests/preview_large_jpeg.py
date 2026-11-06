#!/usr/bin/env python3
"""Real worker: huge baseline/progressive JPEGs, EXIF pixels and CMYK colors.

Usage: python3 tests/preview_large_jpeg.py build/previewcheck build/jpegfixture
Requires Pillow. Generated inputs are read-only test data, not user files.
"""
from pathlib import Path
from PIL import Image, ImageOps
import hashlib
import subprocess as sp
import sys
import tempfile

app, generator = [str(Path(p).resolve()) for p in sys.argv[1:3]]

def digest(path):
    with path.open('rb') as f:
        return hashlib.file_digest(f, 'sha512').digest()

with tempfile.TemporaryDirectory(prefix='ccurate-jpeg-') as temp:
    root = Path(temp)
    def preview(path, ok=True):
        before = digest(path)
        output = root / 'preview.ppm'
        output.unlink(missing_ok=True)
        result = sp.run([app, '--spawn', str(path), '0', str(output)],
                        capture_output=True, text=True, timeout=25)
        assert (result.returncode == 0) == ok, (path.name, result.stdout, result.stderr)
        assert digest(path) == before, 'Input changed'
        if not ok:
            assert '512 MiB preview budget' in result.stdout, result.stdout
            return None
        assert 'Decoder: libjpeg' in result.stdout, result.stdout
        image = Image.open(output).copy()
        assert 0 < image.width <= 1000 and 0 < image.height <= 800
        return image

    for width, height, progressive, ok in [(12000, 8000, 0, True),
            (20000, 10000, 0, True), (12000, 8000, 1, True), (16000, 12000, 1, False)]:
        path = root / f'{width}x{height}-{progressive}.jpg'
        sp.run([generator, path, str(width), str(height), str(progressive)], check=True, timeout=45)
        image = preview(path, ok)
        if image:
            for x, y, rgb in [(.25, .25, (225,25,45)), (.75,.25,(25,25,45)),
                              (.25,.75,(225,225,45)), (.75,.75,(25,225,45))]:
                got = image.getpixel((int(x*image.width),int(y*image.height)))
                assert max(abs(a-b) for a,b in zip(got,rgb)) < 8, got
        print(f'PASS {width}x{height}, progressive={progressive}, expected success={ok}', flush=True)

    pattern = Image.new('RGB', (1600, 1000))
    for box, color in [((0,0,800,500),(230,20,30)), ((800,0,1600,500),(20,220,30)),
                       ((0,500,800,1000),(20,30,230)), ((800,500,1600,1000),(220,220,20))]:
        pattern.paste(color, box)
    for orientation in range(1,9):
        path = root / f'orientation-{orientation}.jpg'
        exif = Image.Exif(); exif[274] = orientation
        pattern.save(path, exif=exif, quality=95)
        actual = preview(path)
        with Image.open(path) as source:
            expected = ImageOps.exif_transpose(source).resize(actual.size)
            for x,y in [(.25,.25),(.75,.25),(.25,.75),(.75,.75)]:
                xy=(int(x*actual.width),int(y*actual.height))
                assert max(abs(a-b) for a,b in zip(actual.getpixel(xy), expected.getpixel(xy))) < 8
    print('PASS EXIF 1-8: rotation and mirroring verified from rendered pixels', flush=True)
    path = root / 'cmyk.jpg'
    pattern.convert('CMYK').save(path, quality=95)
    actual = preview(path)
    with Image.open(path) as source:
        expected = source.convert('RGB').resize(actual.size)
        for xy in [(250,156),(750,156),(250,468),(750,468)]:
            assert max(abs(a-b) for a,b in zip(actual.getpixel(xy),expected.getpixel(xy))) < 8
    print('PASS CMYK rendered colors; all source SHA-512 checks unchanged', flush=True)
