#!/usr/bin/env python3
"""Run the primary Linux regression suites and retain per-suite diagnostics."""
from pathlib import Path
import subprocess
import sys

full, core = (Path(p).resolve() for p in sys.argv[1:3])
logs = Path('test-results')
logs.mkdir(exist_ok=True)
cases = [('integration-full', ['ctest', '--test-dir', str(full), '--output-on-failure']),
         ('integration-core', ['ctest', '--test-dir', str(core), '--output-on-failure'])]
for script, args in [
    ('stress', ['ccurate', 'workercheck']),
    ('live_scan', ['ccurate', 'workercheck']),
    ('sorting', ['ccurate', 'sortcheck']),
    ('preview_process', ['previewcheck']),
    ('preview_large_jpeg', ['previewcheck', 'jpegfixture']),
    ('preview_limits', ['previewcheck', 'libpreview_policy_probe.so']),
    ('desktop_open', ['ccurate']),
    *[(s, ['ccurate']) for s in ('gui_smoke', 'gui_actions', 'gui_activity', 'gui_fonts',
                               'gui_navigation_open', 'gui_scroll_sort', 'gui_startup')],
]:
    cases.append((script, [sys.executable, f'tests/{script}.py', *[str(full / a) for a in args]]))
failures = []
for name, command in cases:
    with (logs / f'{name}.log').open('w') as log:
        result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT)
    print(f'{"PASS" if result.returncode == 0 else "FAIL"}: {name}', flush=True)
    if result.returncode:
        failures.append(name)
        print((logs / f'{name}.log').read_text(errors='replace')[-8000:])
sys.exit(bool(failures))
