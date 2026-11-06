"""Isolated default-app association for generated fixtures; never edits user settings."""
from pathlib import Path
import os
import sys

def desktop_fixture(root):
    root = Path(root)
    data = root / 'desktop-data'
    config = root / 'desktop-config'
    (data / 'applications').mkdir(parents=True)
    config.mkdir()
    recorder = root / 'recorder.py'
    record = root / 'opened.jsonl'
    recorder.write_text('import json, os, sys\nwith open(os.environ["CCURATE_TEST_OPENED"], "a") as f:\n    f.write(json.dumps(sys.argv[1:]) + "\\n")\n')
    types = ['text/plain', 'image/jpeg', 'application/pdf', 'application/octet-stream']
    (data / 'applications/ccurate-test.desktop').write_text(
        '[Desktop Entry]\nType=Application\nName=CCurate test recorder\n'
        f'Exec={sys.executable} {recorder} %f\nTerminal=false\nMimeType={";".join(types)};\n')
    (config / 'mimeapps.list').write_text('[Default Applications]\n' +
        ''.join(f'{mime}=ccurate-test.desktop;\n' for mime in types))
    env = os.environ.copy()
    env.update(XDG_DATA_HOME=str(data), XDG_CONFIG_HOME=str(config),
               CCURATE_TEST_OPENED=str(record), GIO_USE_VFS='local')
    return env, record
