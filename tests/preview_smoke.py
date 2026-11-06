#!/usr/bin/env python3
"""Decode generated trusted PNG/PDF/ZIP/text fixtures; optional FFmpeg JPEG/MP4.

Usage: python3 tests/preview_smoke.py build/previewcheck
This exercises decoders directly, not the privilege-dropping process wrapper.
"""
from pathlib import Path
import binascii
import shutil
import struct
import subprocess
import sys
import tempfile
import zipfile
import zlib

APP = str(Path(sys.argv[1]).resolve())


def png():
    def chunk(kind, data):
        return (struct.pack('!I', len(data)) + kind + data +
                struct.pack('!I', binascii.crc32(kind + data) & 0xffffffff))
    pixels = b''.join(b'\0' + bytes((20, 150, 180)) * 1280 for _ in range(800))
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('!2I5B', 1280, 800, 8, 2, 0, 0, 0)) +
            chunk(b'IDAT', zlib.compress(pixels)) + chunk(b'IEND', b''))


def pdf():
    stream = b'0.1 0.5 0.8 rg 50 50 400 650 re f\n'
    objects = [b'<< /Type /Catalog /Pages 2 0 R >>',
               b'<< /Type /Pages /Kids [3 0 R 4 0 R] /Count 2 >>',
               b'<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] /Resources << >> /Contents 5 0 R >>',
               b'<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] /Resources << >> /Contents 5 0 R >>',
               b'<< /Length ' + str(len(stream)).encode() + b' >>\nstream\n' + stream + b'endstream']
    out = b'%PDF-1.4\n'; offsets = [0]
    for n, obj in enumerate(objects, 1):
        offsets.append(len(out))
        out += f'{n} 0 obj\n'.encode() + obj + b'\nendobj\n'
    xref = len(out)
    out += f'xref\n0 {len(offsets)}\n0000000000 65535 f \n'.encode()
    out += b''.join(f'{offset:010d} 00000 n \n'.encode() for offset in offsets[1:])
    return out + f'trailer\n<< /Size {len(offsets)} /Root 1 0 R >>\nstartxref\n{xref}\n%%EOF\n'.encode()


with tempfile.TemporaryDirectory(prefix='ccurate-preview-') as name:
    root = Path(name)
    (root / 'image.png').write_bytes(png())
    (root / 'document.pdf').write_bytes(pdf())
    (root / 'text.txt').write_text('CCurate preview fixture\n')
    with zipfile.ZipFile(root / 'archive.zip', 'w') as z:
        z.writestr('folder/document.txt', 'retained data')
        z.writestr('../../never-extracted-by-curator.txt', 'must only be listed')

    def check(filename, expected, page=0):
        result = subprocess.run([APP, str(root / filename), str(page)],
                                capture_output=True, timeout=30)
        text = result.stdout.decode(errors='replace')
        assert result.returncode == 0, result.stderr.decode(errors='replace')
        assert expected in text, (filename, text)
        print(f'PASS {filename} page={page}: {expected}')

    check('image.png', 'pixels=1000x625')
    check('document.pdf', 'pages=2')
    check('document.pdf', 'pages=2', 1)
    check('archive.zip', 'folder/document.txt')
    check('text.txt', 'CCurate preview fixture')
    if shutil.which('ffmpeg'):
        subprocess.run(['ffmpeg', '-v', 'error', '-y', '-i', str(root / 'image.png'),
                        '-frames:v', '1', str(root / 'image.jpg')], check=True, timeout=30)
        subprocess.run(['ffmpeg', '-v', 'error', '-y', '-f', 'lavfi', '-i',
                        'color=c=blue:s=640x360:d=2', '-c:v', 'mpeg4',
                        str(root / 'video.mp4')], check=True, timeout=30)
        check('image.jpg', 'pixels=1000x625')
        check('video.mp4', 'pixels=640x360')
    else:
        print('SKIP JPEG/MP4 fixtures: ffmpeg executable is not installed')
