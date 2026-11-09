#!/usr/bin/env python3
"""Run the real view service against a disposable deep-tree catalog."""
from pathlib import Path
import subprocess
import sys
import tempfile

with tempfile.TemporaryDirectory(prefix='ccurate-view-') as directory:
    subprocess.run([sys.executable, str(Path(__file__).with_name('navigation_scale.py')),
                    sys.argv[1], sys.argv[2], directory, '12000'], check=True)
