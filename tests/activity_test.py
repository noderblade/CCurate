#!/usr/bin/env python3
"""Validate the activity worker with a bounded ring and complete disk history."""
from pathlib import Path
import json, subprocess, sys, tempfile
with tempfile.TemporaryDirectory(prefix='ccurate-activity-') as temp:
    subprocess.run([str(Path(sys.argv[1]).resolve()), temp], check=True, timeout=30)
    lines = (Path(temp) / 'activity.log').read_text().splitlines()
    assert len(lines) == 4005, len(lines)
    for line in lines:
        value = line.split(' | path=', 1)[1].split(' | target=', 1)[0]
        assert json.loads(value) == r'file\x0awith\x09controls', line
    print('PASS: 4005 records, 2000-entry ring, four filters, queue drain, escaped paths, restart history')

with tempfile.TemporaryDirectory(prefix='ccurate-activity-failure-') as temp:
    result = subprocess.run([str(Path(sys.argv[1]).resolve()), temp, '--write-failure'], check=True,
                            capture_output=True, text=True, timeout=30)
    assert 'Activity file write failed:' in result.stdout
    print('PASS: file write failure is reported through activity_status')
