#!/usr/bin/env python3
"""Exercise actual restricted Windows preview children, including cancellation."""
from pathlib import Path
import hashlib
import subprocess
import sys
import tempfile
import unittest
import zipfile
from PIL import Image

APP = str(Path(sys.argv.pop(1)).resolve())


class WindowsPreviews(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='ccurate-win-preview-')
        self.root = Path(self.temp.name)

    def tearDown(self):
        self.temp.cleanup()

    def preview(self, path, contains, ok=True, option='--spawn'):
        digest = hashlib.sha512(path.read_bytes()).digest()
        result = subprocess.run([APP, option, str(path)], capture_output=True, timeout=30)
        output = (result.stdout + result.stderr).decode('utf-8', 'replace')
        self.assertEqual(result.returncode == 0, ok, output)
        self.assertIn(contains, output)
        self.assertEqual(hashlib.sha512(path.read_bytes()).digest(), digest)

    def test_text_and_binary(self):
        text = self.root / 'tekst_żółć.txt'
        text.write_text('Important text document.\n', encoding='utf-8')
        self.preview(text, 'Important text document.')
        binary = self.root / 'binary.bin'
        binary.write_bytes(bytes(range(256)))
        self.preview(binary, '00 01 02 03')

    def test_png_and_jpeg(self):
        for extension in ('png', 'jpg'):
            path = self.root / ('photo.' + extension)
            Image.new('RGB', (1200, 600), (35, 160, 80)).save(path)
            self.preview(path, 'pixels=1000x500')

    def test_large_jpeg(self):
        path = self.root / 'large.jpg'
        with Image.new('RGB', (12000, 8000), (40, 90, 160)) as image:
            image.save(path, quality=85)
        self.preview(path, 'pixels=1000x666')

    def test_archive_listing(self):
        path = self.root / 'archive.zip'
        with zipfile.ZipFile(path, 'w') as archive:
            archive.writestr('project/document.txt', 'Important archive member.')
        self.preview(path, 'project/document.txt')
        self.assertFalse((self.root / 'project').exists())

    def test_pdf(self):
        objects = [b'<< /Type /Catalog /Pages 2 0 R >>',
                   b'<< /Type /Pages /Kids [3 0 R] /Count 1 >>',
                   b'<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 100] /Contents 4 0 R >>',
                   b'<< /Length 0 >>\nstream\n\nendstream']
        pdf = bytearray(b'%PDF-1.4\n'); offsets = []
        for i, body in enumerate(objects, 1):
            offsets.append(len(pdf)); pdf += f'{i} 0 obj\n'.encode() + body + b'\nendobj\n'
        xref = len(pdf)
        pdf += b'xref\n0 5\n0000000000 65535 f \n'
        for offset in offsets:
            pdf += f'{offset:010d} 00000 n \n'.encode()
        pdf += f'trailer\n<< /Size 5 /Root 1 0 R >>\nstartxref\n{xref}\n%%EOF\n'.encode()
        path = self.root / 'document.pdf'; path.write_bytes(pdf)
        self.preview(path, 'PDF  |  page 1 / 1')

    def test_cancellation(self):
        path = self.root / 'cancel.txt'; path.write_bytes(b'cancelled fixture')
        self.preview(path, 'cancelled by a newer selection', ok=False, option='--cancel')

    def test_unrestricted_worker_is_refused(self):
        result = subprocess.run([APP, '--preview-worker', '0', '0', '0'], capture_output=True, timeout=10)
        self.assertIn(b'Windows worker restrictions are not active', result.stdout)


if __name__ == '__main__':
    unittest.main(verbosity=2)
