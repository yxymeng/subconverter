#!/usr/bin/env python3
"""Check the final container, static UI, and one conversion without external sources."""
import argparse
import base64
import json
from pathlib import Path
import re
import subprocess
import tempfile
import time
from urllib.parse import urlencode
from urllib.error import HTTPError
from urllib.request import build_opener, ProxyHandler

parser = argparse.ArgumentParser()
parser.add_argument('--image', required=True)
parser.add_argument('--commit', required=True)
args = parser.parse_args()
version = re.search(r'^#define VERSION "([^"]+)"',
                    (Path(__file__).resolve().parents[1]/'src/version.h').read_text(), re.M).group(1)

def docker(*arguments):
    return subprocess.check_output(['docker', *arguments], text=True).strip()

check = docker('run', '--rm', args.image, './subconverter', '-f', 'pref.example.toml', '--check')
report = json.loads(check[check.index('{'):])
assert report['build_commit'] == args.commit, report
assert report['version'] == version, report
with tempfile.TemporaryDirectory(prefix='subconverter-smoke-') as directory:
    pref = Path(directory)/'smoke.ini'
    pref.write_text('[common]\napi_mode=true\nenable_insert=false\nproxy_subscription=NONE\n'
                    '[ruleset]\nenabled=false\n[server]\nlisten=0.0.0.0\nport=25500\nserve_file_root=web\n')
    container = docker('run', '-d', '--rm', '-p', '127.0.0.1::25500',
                       '--mount', f'type=bind,src={pref},dst=/opt/subconverter/smoke.ini,readonly',
                       args.image, './subconverter', '-f', 'smoke.ini')
    try:
        endpoint = docker('port', container, '25500/tcp')
        origin = 'http://'+endpoint
        opener = build_opener(ProxyHandler({}))
        def get(path):
            try:
                with opener.open(origin+path, timeout=5) as response:
                    assert response.status == 200
                    return response.read()
            except HTTPError as error:
                raise RuntimeError(str(error)+': '+error.read().decode()) from error
        for _ in range(100):
            try:
                status = json.loads(get('/status'))
                break
            except OSError: time.sleep(.1)
        else: raise RuntimeError('Container did not become ready')
        assert status['build_commit'] == args.commit, status
        assert b'converter.js' in get('/')
        assert b'const api = {build, restore}' in get('/converter.js')
        subscription = base64.b64encode(b'trojan://smoke-secret@127.0.0.2:443#container-smoke').decode()
        output = get('/sub?'+urlencode({'target':'trojan', 'url':'data:,'+subscription, 'emoji':'false'}))
        assert b'trojan://smoke-secret@127.0.0.2:443' in base64.b64decode(output)
        print(json.dumps({'version':report['version'], 'build_commit':args.commit,
                          'status':True, 'web':True, 'conversion':True}))
    finally:
        docker('stop', container)
