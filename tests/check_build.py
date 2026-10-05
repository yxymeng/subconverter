#!/usr/bin/env python3
"""Reject release artifacts whose embedded source identity or version is wrong."""
import argparse
import json
from pathlib import Path
import re
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument('--binary', required=True, type=Path)
parser.add_argument('--commit', required=True)
parser.add_argument('--version-header', type=Path, default=Path('src/version.h'))
args = parser.parse_args()
binary = args.binary.resolve()
version = re.search(r'^#define VERSION "([^"]+)"', args.version_header.read_text(), re.M).group(1)
result = subprocess.run([str(binary), '-f', str(binary.parent/'pref.example.toml'), '--check'],
                        check=True, capture_output=True, text=True, timeout=30)
report = json.loads(result.stdout[result.stdout.index('{'):])
if not re.fullmatch(r'[0-9a-f]{40}', args.commit):
    raise SystemExit('Expected a full source commit SHA')
if report['build_commit'] != args.commit or report['version'] != version:
    raise SystemExit('Build identity mismatch: '+json.dumps(report))
print(json.dumps({'version':report['version'], 'build_commit':report['build_commit']}))
