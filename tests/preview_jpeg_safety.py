#!/usr/bin/env python3
"""Direct-decoder checks for ASan/UBSan builds; generated trusted fixtures only.
The production GUI always uses the limited preview subprocess instead.
Usage: python3 tests/preview_jpeg_safety.py build-preview-asan/previewcheck
"""
from pathlib import Path
from PIL import Image
import hashlib
import random
import struct
import subprocess as sp
import sys
import tempfile

app = str(Path(sys.argv[1]).resolve())
random_ = random.Random(542)
with tempfile.TemporaryDirectory(prefix='ccurate-jpeg-safety-') as temp:
    root = Path(temp); path = root/'fixture.jpg'
    Image.new('RGB', (1280,800),(170,40,90)).save(path)
    original = path.read_bytes()
    cases = [b'', b'Exif\0\0', b'Exif\0\0II*\0\xff\xff\xff\xff',
             b'Exif\0\0MM\0*\xff\xff\xff\xff',
             b'Exif\0\0II*\0\x08\0\0\0\xff\xff']
    for n in range(32):
        cases.append(b'Exif\0\0'+random_.randbytes(n*7))
    for orientation in range(1,9):
        for endian, marker in [('<',b'II'),('>',b'MM')]:
            tiff = marker+struct.pack(endian+'HI',42,8)+struct.pack(endian+'H',1)
            tiff += struct.pack(endian+'HHIHHI',0x112,3,1,orientation,0,0)
            cases.append(b'Exif\0\0'+tiff)
    for index, payload in enumerate(cases):
        path.write_bytes(original[:2]+b'\xff\xe1'+struct.pack('>H',len(payload)+2)+payload+original[2:])
        before=hashlib.sha512(path.read_bytes()).digest()
        result=sp.run([app,path],capture_output=True,text=True,errors='replace',timeout=10)
        assert result.returncode==0,(index,result.stdout,result.stderr)
        assert 'Decoder: libjpeg' in result.stdout,(index,result.stdout)
        assert 'Sanitizer' not in result.stderr,result.stderr
        assert hashlib.sha512(path.read_bytes()).digest()==before
    for length in [2,16,100,500]:
        path.write_bytes(original[:length])
        result=sp.run([app,path],capture_output=True,text=True,errors='replace',timeout=10)
        assert result.returncode in (0,1),result.stderr
        assert 'Sanitizer' not in result.stderr,result.stderr
    print(f'PASS {len(cases)} valid/malformed EXIF payloads (both endian orders) and 4 truncated JPEGs')
