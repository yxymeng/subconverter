#!/usr/bin/env python3
"""Behavior regressions against the complete binary; sources never leave localhost."""
import argparse
import base64
from collections import Counter
from concurrent.futures import ThreadPoolExecutor
import json
import http.client
import os
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import shutil
import socket
import subprocess
import sys
import struct
import tempfile
import threading
import time
import unittest
from urllib.error import HTTPError
from urllib.parse import urlencode, urlsplit
from urllib.request import Request, build_opener, ProxyHandler

OPENER = build_opener(ProxyHandler({}))
SUB = base64.b64encode(b'trojan://fixture-password@127.0.0.2:443#fixture-node')
BINARY = None
BASE = None

def get(url, headers=None):
    try:
        with OPENER.open(Request(url, headers=headers or {}), timeout=30) as response:
            return response.status, response.read(), dict(response.headers)
    except HTTPError as error:
        return error.code, error.read(), dict(error.headers)

class Source:
    def __init__(self):
        self.lock = threading.Lock()
        self.counts = Counter()
        self.headers = {}
        self.routes = {}
        self.response_headers = {}
        self.active = 0
        self.peak = 0
        owner = self
        class Handler(BaseHTTPRequestHandler):
            protocol_version = 'HTTP/1.1'
            def log_message(self, *args): pass
            def do_GET(self):
                path = urlsplit(self.path).path
                with owner.lock:
                    owner.counts[path] += 1
                    number = owner.counts[path]
                    owner.headers[path] = dict(self.headers)
                    owner.active += 1
                    owner.peak = max(owner.peak, owner.active)
                try:
                    route = owner.routes.get(path, (200, SUB, 0))
                    if callable(route): route = route(number, self.headers)
                    code, body, delay = route[:3]
                    time.sleep(delay)
                    self.send_response(code)
                    self.send_header('Content-Length', str(len(body) + (100 if len(route) > 3 and route[3] == 'truncate' else 0)))
                    headers=route[3] if len(route)>3 and isinstance(route[3],dict) else owner.response_headers
                    for key,value in headers.items(): self.send_header(key,value)
                    self.end_headers()
                    try: self.wfile.write(body)
                    except (BrokenPipeError, ConnectionResetError): pass
                    if len(route) > 3 and route[3] == 'truncate': self.close_connection = True
                finally:
                    with owner.lock: owner.active -= 1
        self.server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        self.server.daemon_threads = True
        self.origin = 'http://127.0.0.1:' + str(self.server.server_port)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
    def close(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join()

class App:
    def __init__(self, extra='', asynchronous=True, parallel=4, proxy='NONE', default_url='',
                 listen='127.0.0.1', fallback=True, template='base/fixture.yml', workers=8,
                 port=None, origin_host='127.0.0.1'):
        self.temp = tempfile.TemporaryDirectory(prefix='subconverter-test-')
        self.root = Path(self.temp.name)
        shutil.copytree(BASE, self.root, dirs_exist_ok=True)
        (self.root / 'base' / 'fixture.yml').write_text('port: 7890\nproxies: []\nproxy-groups: []\nrules: []\n')
        if port is None:
            with socket.socket() as s:
                s.bind(('127.0.0.1', 0)); port = s.getsockname()[1]
        self.port = port
        self.pref = self.root / 'fixture.ini'
        self.pref.write_text(f'''[common]
api_mode=true
default_url={default_url}
enable_insert=false
clash_rule_base={template}
proxy_config=NONE
proxy_ruleset=NONE
proxy_subscription={proxy}
[node_pref]
clash_use_new_field_name=true
[ruleset]
enabled=true
overwrite_original_rules=true
[server]
listen={listen}
port={self.port}
serve_file_root=web
[advanced]
log_level=verbose
max_allowed_rules=0
max_allowed_download_size=0
max_concurrent_threads={workers}
enable_cache=true
cache_subscription=60
cache_ruleset=60
cache_config=60
serve_cache_on_fetch_fail={str(fallback).lower()}
async_fetch_ruleset={str(asynchronous).lower()}
max_parallel_downloads={parallel}
connect_timeout=1
download_timeout=1
skip_failed_links=true
{extra}
''')
        self.log = open(self.root / 'stderr.log', 'wb')
        self.process = subprocess.Popen([str(BINARY), '-f', str(self.pref)], stdout=subprocess.DEVNULL, stderr=self.log)
        self.origin = 'http://' + origin_host + ':' + str(self.port)
        for _ in range(100):
            if self.process.poll() is not None: raise RuntimeError((self.root/'stderr.log').read_text())
            try:
                if get(self.origin + '/status')[0] == 200: break
            except OSError: pass
            time.sleep(.05)
        else: raise RuntimeError('server did not start')
    def request(self, source, config=None, headers=None, **params):
        query = {'target':'clash', 'url':source, 'emoji':'false', **params}
        if config: query['config'] = config
        status, raw, response_headers = get(self.origin + '/diagnose?' + urlencode(query), headers)
        return status, json.loads(raw), response_headers
    def close(self):
        self.process.terminate()
        try: self.process.wait(timeout=8)
        except subprocess.TimeoutExpired: self.process.kill(); self.process.wait()
        self.log.close()
        self.temp.cleanup()

class Integration(unittest.TestCase):
    def setUp(self): self.source = Source(); self.apps=[]
    def tearDown(self):
        for app in self.apps: app.close()
        self.source.close()
    def app(self, **kwargs):
        app=App(**kwargs); self.apps.append(app); return app
    def config(self, urls):
        self.source.routes['/config'] = (200, ('[custom]\nenable_rule_generator=true\noverwrite_original_rules=true\n' + ''.join('ruleset=DIRECT,'+url+'\n' for url in urls) + 'ruleset=DIRECT,[]MATCH\n').encode(), 0)
        return self.source.origin+'/config'
    def expire(self, app):
        old=time.time()-120
        for entry in (app.root/'cache').glob('v2-*'): os.utime(entry,(old,old))
    def generate(self, app, sections, file_size_limit=None):
        (app.root/'generate.ini').write_text(''.join('['+name+']\n'+''.join(key+'='+value+'\n' for key,value in items.items()) for name,items in sections))
        command=[str(BINARY),'-f',str(app.pref),'-g']
        if file_size_limit is not None:
            command=[sys.executable,'-c','import os,resource,signal,sys; signal.signal(signal.SIGXFSZ,signal.SIG_IGN); resource.setrlimit(resource.RLIMIT_FSIZE,(int(sys.argv[1]),int(sys.argv[1]))); os.execv(sys.argv[2],sys.argv[2:])',str(file_size_limit),*command]
        return subprocess.run(command,cwd=app.root,capture_output=True,text=True,timeout=15)
    def test_header_policy_and_403_fix(self):
        self.source.routes['/sub'] = lambda n,h: (403 if 'SubConverter-Request' in h or 'SubConverter-Version' in h else 200, SUB, 0)
        self.source.response_headers={'Set-Cookie':'arbitrary_secret=fixture-cookie'}
        app=self.app(extra='subscription_source_headers='+json.dumps({self.source.origin:{'X-Source-Key':'source-secret'}}))
        status, report, headers = app.request(self.source.origin+'/sub?token=fake-token', headers={'User-Agent':'fixture-UA','Cookie':'incoming-secret','Authorization':'Bearer incoming-token','X-Custom':'unrelated'})
        self.assertEqual(status,200,report)
        sent=self.source.headers['/sub']
        self.assertEqual(sent['User-Agent'],'fixture-UA')
        self.assertEqual(sent['X-Source-Key'],'source-secret')
        self.assertNotIn('Cookie',sent); self.assertNotIn('Authorization',sent); self.assertNotIn('X-Custom',sent)
        self.assertEqual(headers['X-Request-ID'],report['request_id'])
        diagnostic=json.dumps({k:v for k,v in report.items() if k!='output'})
        log=(app.root/'stderr.log').read_text()
        for secret in ['fake-token','incoming-secret','incoming-token','source-secret']:
            self.assertNotIn(secret,diagnostic); self.assertNotIn(secret,log)
        # http.client sends no User-Agent, exercising the default outbound fallback.
        connection=http.client.HTTPConnection('127.0.0.1',app.port,timeout=10)
        connection.request('GET','/diagnose?'+urlencode({'target':'clash','url':self.source.origin+'/fallback'}))
        response=connection.getresponse();data=json.loads(response.read());connection.close()
        self.assertEqual(response.status,200,data)
        self.assertTrue(self.source.headers['/fallback']['User-Agent'].startswith('subconverter/'))
        self.assertNotIn('Cookie',self.source.headers['/fallback'])
        app.request(self.source.origin+'/again',refresh='true')
        self.assertNotIn('Cookie',self.source.headers['/again'])
        self.assertNotIn('fixture-cookie',(app.root/'stderr.log').read_text())

    def test_distinct_errors_and_retry_buffer(self):
        app=self.app()
        for path, route, expected in [('/forbidden',(403,b'denied',0),'HTTP 403'),('/invalid',(200,b'not a subscription',0),'cannot be parsed'),('/timeout',(200,SUB,1.2),'Timeout')]:
            self.source.routes[path]=route
            status,report,_=app.request(self.source.origin+path)
            self.assertGreaterEqual(status,400,report)
            if expected=='Timeout': self.assertIn('Timeout',str(report['downloads']))
            else: self.assertIn(expected,report['error'])
        status,report,_=app.request(self.source.origin+'/not-exportable',target='ss')
        self.assertEqual(status,422,report);self.assertIn('exported',report['error'])
        self.source.routes['/retry']=lambda n,h: (200,b'INVALID-PREFIX',0,'truncate') if n==1 else (200,SUB,0)
        status,report,_=app.request(self.source.origin+'/retry')
        self.assertEqual(status,200,report)
        self.assertEqual(report['downloads'][0]['attempts'],2)
        self.assertEqual(self.source.counts['/retry'],2)
    def test_failed_update_preserves_cache_and_manual_stale(self):
        app=self.app()
        rule=self.source.origin+'/rule'
        config=self.config([rule])
        self.source.routes['/rule']=(200,b'DOMAIN,old.example\n',0)
        status,report,_=app.request(self.source.origin+'/sub',config)
        self.assertEqual(status,200,report); self.assertIn('old.example',report['output'])
        for route in [(200,b'PARTIAL-SECRET',0,'truncate'),
                      (200,b'<html>upstream unavailable</html>',0),
                      (200,b'{"error":"unavailable"}',0),
                      (200,b'DOMAIN,\n',0),
                      (200,b'IP-CIDR,192.0.2.0/4294967296\n',0),
                      (200,b'IP-CIDR6,1:2:3:4:5:6:7:8::9/64\n',0),
                      (200,b'SRC-IP-CIDR,<html>error</html>\n',0)]:
            self.source.routes['/rule']=route
            status,report,_=app.request(self.source.origin+'/sub',config,refresh='true')
            self.assertEqual(status,502,report); self.assertEqual(report['output'],'')
            self.assertIn('Required ruleset',report['error'])
            if len(route)==3: self.assertIn('Invalid ruleset content',str(report['downloads']))
            status,report,_=app.request(self.source.origin+'/sub',config,use_stale='true')
            self.assertEqual(status,200,report); self.assertIn('old.example',report['output'])
            self.assertNotIn('PARTIAL',report['output'])
            self.assertIn('stale',[d['cache'] for d in report['downloads']])

    def test_ruleset_formats_and_empty_payload(self):
        app=self.app()
        rule=self.source.origin+'/rule'
        for prefix,body,expected in [('',b'# comment\nDOMAIN,valid.example // note\n','valid.example'),
                                     ('quanx:',b'host,valid.example,DIRECT\n','valid.example'),
                                     ('clash-domain:',b'payload:\n  - +.valid.example\n','valid.example'),
                                     ('clash-ipcidr:',b'payload:\n  - 192.0.2.0/24\n','192.0.2.0/24'),
                                     ('',b'IP-CIDR6,::ffff:192.0.2.0/128\n','::ffff:192.0.2.0/128'),
                                     ('',b'SRC-IP-CIDR,2001:db8::/32\n','2001:db8::/32'),
                                     ('clash-classic:',b'payload:\n  - DOMAIN,valid.example\n','valid.example'),
                                     ('clash-domain:',b'payload: []\n','MATCH,DIRECT'),
                                     ('',b'# intentionally empty ruleset\n','MATCH,DIRECT')]:
            self.source.routes['/rule']=(200,body,0)
            config=self.config([prefix+rule])
            status,report,_=app.request(self.source.origin+'/sub',config,refresh='true')
            self.assertEqual(status,200,report); self.assertIn(expected,report['output'])
        for prefix,body in [('clash-domain:',b'payload:\n  - <html>error</html>\n'),
                            ('clash-ipcidr:',b'payload:\n  - valid.example\n'),
                            ('clash-classic:',b'payload:\n  - {error: unavailable}\n')]:
            self.source.routes['/rule']=(200,body,0)
            status,report,_=app.request(self.source.origin+'/sub',self.config([prefix+rule]),refresh='true')
            self.assertEqual(status,502,report)

    def test_refresh_takes_priority_over_manual_stale(self):
        app=self.app()
        config=self.config([self.source.origin+'/rule'])
        self.source.routes['/rule']=(200,b'DOMAIN,old.example\n',0)
        self.assertEqual(app.request(self.source.origin+'/sub',config)[0],200)
        before=self.source.counts['/rule']
        self.source.routes['/rule']=(200,b'DOMAIN,new.example\n',0)
        self.expire(app)
        status,report,_=app.request(self.source.origin+'/sub',config,use_stale='true')
        self.assertEqual(status,200,report); self.assertIn('old.example',report['output'])
        self.assertEqual(self.source.counts['/rule'],before)
        status,report,_=app.request(self.source.origin+'/sub',config,refresh='true',use_stale='true')
        self.assertEqual(status,200,report); self.assertIn('new.example',report['output'])
        self.assertNotIn('old.example',report['output'])
        self.assertEqual(self.source.counts['/rule'],before+1)
        self.source.routes['/rule']=(403,b'denied',0)
        status,report,_=app.request(self.source.origin+'/sub',config,refresh='true',use_stale='true')
        self.assertEqual(status,200,report); self.assertIn('new.example',report['output'])
        self.assertEqual(self.source.counts['/rule'],before+2)
        self.assertIn('stale',[d['cache'] for d in report['downloads']])
        status,report,_=app.request(self.source.origin+'/sub',config,refresh='true')
        self.assertEqual(status,502,report); self.assertEqual(report['output'],'')
        self.source.routes['/uncached-rule']=(403,b'denied',0)
        status,report,_=app.request(self.source.origin+'/sub',self.config([self.source.origin+'/uncached-rule']),refresh='true',use_stale='true')
        self.assertEqual(status,502,report); self.assertEqual(report['output'],'')

    def test_non_rule_fallback_obeys_legacy_setting(self):
        for fallback in (True,False):
            template=self.source.origin+'/template'
            app=self.app(fallback=fallback,template=template)
            config=self.config([])
            for path in ('/sub','/config','/template'):
                with self.subTest(fallback=fallback,resource=path):
                    self.source.routes['/sub']=(200,SUB,0)
                    self.config([])
                    self.source.routes['/template']=(200,b'port: 4321\nfixture_marker: cached-template\nproxies: []\nproxy-groups: []\nrules: []\n',0)
                    status,report,_=app.request(self.source.origin+'/sub',config,refresh='true')
                    self.assertEqual(status,200,report); self.assertIn('cached-template',report['output'])
                    self.expire(app)
                    self.source.routes[path]=(403,b'denied',.2)
                    before=self.source.counts[path]
                    with ThreadPoolExecutor(max_workers=4) as pool:
                        results=list(pool.map(lambda _:app.request(self.source.origin+'/sub',config),range(4)))
                    self.assertEqual(self.source.counts[path],before+1)
                    for status,report,_ in results:
                        if fallback:
                            self.assertEqual(status,200,report)
                            self.assertIn('cached-template',report['output']); self.assertIn('port: 4321',report['output'])
                            self.assertIn('stale',[d['cache'] for d in report['downloads']])
                        else:
                            self.assertGreaterEqual(status,400,report)
                            self.assertFalse(report['success']); self.assertEqual(report['output'],'')
                            self.assertTrue(report['error'])

    def test_source_headers_redirect_boundary(self):
        destination=Source()
        try:
            # Verify rejection without masking the error through an allowed cached subscription.
            app=self.app(fallback=False,extra='subscription_source_headers='+json.dumps({self.source.origin:{'X-Source-Key':'source-secret'}}))
            self.source.routes['/redirect']=(302,b'',0)
            self.source.response_headers={'Location':'/same-origin'}
            status,report,_=app.request(self.source.origin+'/redirect')
            self.assertEqual(status,200,report)
            self.assertEqual(self.source.headers['/same-origin']['X-Source-Key'],'source-secret')
            self.source.response_headers={'Location':destination.origin+'/destination'}
            status,report,_=app.request(self.source.origin+'/redirect',refresh='true')
            self.assertGreaterEqual(status,400,report)
            self.assertIn('Cross-origin redirect',str(report))
            self.assertEqual(destination.counts['/destination'],0)
            self.assertEqual([d['attempts'] for d in report['downloads']],[1])
            # Ordinary subscriptions retain cross-origin redirects without dedicated source headers.
            plain=self.app()
            status,report,_=plain.request(self.source.origin+'/redirect')
            self.assertEqual(status,200,report)
            self.assertNotIn('X-Source-Key',destination.headers['/destination'])
        finally: destination.close()
    def test_ttl_force_identity_and_shared_requests(self):
        app=self.app()
        config=self.config([self.source.origin+'/rule'])
        self.source.routes['/rule']=(200,b'DOMAIN,first.example\n',.15)
        source=self.source.origin+'/sub'
        first=app.request(source,config)[1]
        self.assertTrue(first['success'],first)
        counts=self.source.counts.copy()
        self.assertEqual(first['output'],app.request(source,config)[1]['output'])
        self.assertEqual(counts,self.source.counts)
        self.source.routes['/rule']=(200,b'DOMAIN,second.example\n',.15)
        self.expire(app)
        self.assertIn('second.example',app.request(source,config)[1]['output'])
        self.assertGreater(self.source.counts['/rule'],counts['/rule'])
        self.assertEqual(app.request(source,config,refresh='true')[0],200)
        self.source.routes['/unique']=(200,SUB,.25)
        with ThreadPoolExecutor(max_workers=4) as pool:
            results=list(pool.map(lambda _:app.request(self.source.origin+'/unique'),range(4)))
        self.assertTrue(all(r[0]==200 for r in results),results)
        self.assertEqual(self.source.counts['/unique'],1)
        self.source.routes['/concurrent-failure']=(403,b'denied',.25)
        with ThreadPoolExecutor(max_workers=4) as pool:
            failures=list(pool.map(lambda _:app.request(self.source.origin+'/concurrent-failure'),range(4)))
        self.assertEqual(self.source.counts['/concurrent-failure'],1)
        for status,report,_ in failures:
            self.assertGreaterEqual(status,400)
            self.assertIn(403,[entry['http_status'] for entry in report['downloads']])
        app.request(source,headers={'User-Agent':'UA-A'})
        app.request(source,headers={'User-Agent':'UA-B'})
        self.assertEqual(self.source.counts['/sub'],5)  # cold, expired, force, two distinct UAs
    def test_bounded_parallelism_and_rule_order(self):
        app=self.app(parallel=3)
        urls=[]
        for i in range(12):
            path=f'/rule-{i}';urls.append(self.source.origin+path)
            self.source.routes[path]=(200,f'DOMAIN,rule-{i}.example\n'.encode(),.08)
        config=self.config(urls)
        status,report,_=app.request(self.source.origin+'/sub',config)
        self.assertEqual(status,200,report)
        self.assertLessEqual(self.source.peak,3); self.assertGreater(self.source.peak,1)
        positions=[report['output'].index(f'rule-{i}.example') for i in range(12)]
        self.assertEqual(positions,sorted(positions))
    def test_startup_check_port_conflict_and_bad_config(self):
        app=self.app()
        result=subprocess.run([str(BINARY),'-f',str(app.pref),'--check'],capture_output=True,text=True)
        self.assertEqual(result.returncode,0,result.stderr)
        report=json.loads(result.stdout[result.stdout.index('{'):])
        self.assertEqual(report['config'],str(app.pref));self.assertEqual(report['proxies']['subscription']['mode'],'direct')
        collision=subprocess.run([str(BINARY),'-f',str(app.pref)],capture_output=True,text=True,timeout=10)
        self.assertNotEqual(collision.returncode,0); self.assertIn('Cannot bind',collision.stderr)
        for name,body in [('bad.toml','version = \n[common]'),('bad.ini','[common]\n[advanced]\nsubscription_source_headers=[]')]:
            path=app.root/name;path.write_text(body)
            bad=subprocess.run([str(BINARY),'-f',str(path),'--check'],capture_output=True,text=True,timeout=10)
            self.assertNotEqual(bad.returncode,0,bad.stdout)
    def test_proxy_failure_and_self_request(self):
        app=self.app()
        broken=self.app(proxy='http://127.0.0.1:1')
        status,report,_=broken.request(self.source.origin+'/sub')
        self.assertGreaterEqual(status,400,report)
        # A refused proxy connection can reach the connect timeout on Windows.
        self.assertIn(report['downloads'][0]['transport_code'],(7,28),report)
        self.assertEqual(self.source.counts['/sub'],0)
        self.assertIn('proxy',str(report['downloads']))
        status,report,_=app.request(app.origin+'/sub?target=clash')
        self.assertGreaterEqual(status,400);self.assertIn('Self-referencing',str(report))
    def test_self_request_resolves_wildcard_aliases(self):
        app=self.app(listen='0.0.0.0',parallel=2)
        aliases=['127.0.0.2','localhost','[::ffff:127.0.0.1]']
        try:
            addresses=socket.getaddrinfo(socket.gethostname(),None,socket.AF_INET)
            aliases.extend({address[4][0] for address in addresses})
            aliases.append(socket.gethostname())
        except socket.gaierror: pass
        for host in aliases:
            with self.subTest(host=host):
                status,report,_=app.request(f'http://{host}:{app.port}/sub?target=clash')
                self.assertGreaterEqual(status,400,report)
                self.assertIn('Self-referencing',str(report))
                self.assertEqual(report['downloads'][0]['attempts'],1)
                self.assertEqual(get(app.origin+'/status')[0],200)
        bound=self.app()
        status,report,_=bound.request(f'http://localhost:{bound.port}/sub?target=clash')
        self.assertGreaterEqual(status,400,report); self.assertIn('Self-referencing',str(report))
    def test_failed_configuration_reload_preserves_previous_settings(self):
        for mode in ('readconf','updateconf','automatic'):
            app=self.app(parallel=2,extra='subscription_source_headers='+json.dumps({self.source.origin:{'X-Policy':'old'}}))
            original=app.pref.read_text()
            if mode=='automatic':
                original=original.replace('api_mode=true','api_mode=false').replace('[common]\n','[common]\nreload_conf_on_request=true\n')
                app.pref.write_text(original)
                self.assertEqual(get(app.origin+'/readconf')[0],200)
            expected=json.loads(get(app.origin+'/status')[1])
            invalid={
                'ini':original.replace('listen=127.0.0.1','listen=127.0.0.2').replace('max_parallel_downloads=2','max_parallel_downloads=7').replace('subscription_source_headers='+json.dumps({self.source.origin:{'X-Policy':'old'}}),'subscription_source_headers=[]'),
                'toml':'version = 1\n[common]\napi_mode=true\nproxy_subscription="http://127.0.0.1:1"\n[server]\nlisten="127.0.0.2"\n[advanced]\nmax_parallel_downloads=7\nsubscription_source_headers="[]"\n',
                'yaml':'common:\n  api_mode: true\n  proxy_subscription: http://127.0.0.1:1\nserver:\n  listen: 127.0.0.2\nadvanced:\n  max_parallel_downloads: 7\n  subscription_source_headers: "[]"\n'
            }
            invalid['yaml-type']=invalid['yaml'].replace('subscription_source_headers: "[]"','subscription_source_headers: []')
            invalid['toml-type']=invalid['toml'].replace('subscription_source_headers="[]"','subscription_source_headers=[]')
            for format,body in invalid.items():
                with self.subTest(mode=mode,format=format):
                    counts=self.source.counts.copy()
                    if mode=='updateconf':
                        connection=http.client.HTTPConnection('127.0.0.1',app.port,timeout=10)
                        connection.request('POST','/updateconf?type=direct',body.encode())
                        response=connection.getresponse(); status=response.status; error=response.read(); connection.close()
                        self.assertEqual(app.pref.read_text(),original)
                    else:
                        app.pref.write_text(body)
                        if mode=='readconf': status,error,_=get(app.origin+'/readconf')
                        else:
                            status,report,_=app.request(self.source.origin+'/blocked-'+format,refresh='true')
                            error=str(report).encode()
                    self.assertEqual(status,400,error)
                    self.assertIn(b'Failed to reload configuration',error)
                    self.assertEqual(self.source.counts,counts)
                    self.assertEqual(json.loads(get(app.origin+'/status')[1]),expected)
                    if mode=='automatic': app.pref.write_text(original)
                    source=self.source.origin+'/retained-'+mode+'-'+format
                    status,report,_=app.request(source,refresh='true')
                    self.assertEqual(status,200,report)
                    self.assertEqual(self.source.headers[urlsplit(source).path]['X-Policy'],'old')
                    app.pref.write_text(original)
            if mode=='updateconf':
                updated=original.replace('"old"','"new"').replace('max_parallel_downloads=2','max_parallel_downloads=3')
                self.source.routes['/import-rules']=(200,b'DIRECT,[]MATCH\n',0)
                updated=updated.replace('proxy_config=NONE','proxy_config='+self.source.origin).replace('[ruleset]\n','[ruleset]\nruleset=!!import:http://127.0.0.2:1/import-rules\n')
                updated+='\n# common: this is an INI comment\n'
                connection=http.client.HTTPConnection('127.0.0.1',app.port,timeout=10)
                connection.request('POST','/updateconf?type=direct',updated.encode())
                response=connection.getresponse(); self.assertEqual(response.status,200,response.read()); connection.close()
                self.assertEqual(app.pref.read_text(),updated)
                self.assertEqual(json.loads(get(app.origin+'/status')[1])['max_parallel_downloads'],3)
                self.assertEqual(self.source.counts['/import-rules'],1)
                status,report,_=app.request(self.source.origin+'/updated-policy',refresh='true')
                self.assertEqual(status,200,report)
                self.assertEqual(self.source.headers['/updated-policy']['X-Policy'],'new')

    def test_self_request_uses_concrete_hostname_binding(self):
        addresses={address[4][0] for address in socket.getaddrinfo('localhost',None,socket.AF_UNSPEC,socket.SOCK_STREAM)}
        if not {'127.0.0.1','::1'}.issubset(addresses): self.skipTest('localhost does not resolve to both loopback families')
        try:
            with socket.socket(socket.AF_INET6,socket.SOCK_STREAM) as probe:
                probe.bind(('::1',self.source.server.server_port))
        except OSError as error: self.skipTest('IPv6 loopback unavailable: '+str(error))
        app=self.app(listen='localhost',port=self.source.server.server_port,origin_host='[::1]',workers=2)
        status,report,_=app.request(self.source.origin+'/sub',refresh='true')
        self.assertEqual(status,200,report)
        self.assertIn('fixture-node',report['output'])
        self.assertEqual(self.source.counts['/sub'],1)
        destination=app.origin+'/sub?target=clash'
        self.source.routes['/bound-redirect']=(302,b'',0,{'Location':destination})
        for source in (destination,self.source.origin+'/bound-redirect'):
            status,report,_=app.request(source,refresh='true')
            self.assertGreaterEqual(status,400,report)
            self.assertIn('Self-referencing',str(report))
            self.assertEqual(report['downloads'][0]['attempts'],1)
        self.assertEqual(self.source.counts['/bound-redirect'],1)
        self.assertEqual(get(app.origin+'/status')[0],200)

    def test_self_request_keeps_bound_endpoint_after_config_reload(self):
        for automatic in (False,True):
            app=self.app(listen='0.0.0.0',parallel=2,fallback=False,workers=2)
            original=app.pref.read_text()
            if automatic:
                original=original.replace('api_mode=true','api_mode=false').replace('[common]\n','[common]\nreload_conf_on_request=true\n')
                app.pref.write_text(original)
                self.assertEqual(get(app.origin+'/readconf')[0],200)
            for change_address,change_port in ((True,False),(False,True),(True,True)):
                with self.subTest(automatic=automatic,address=change_address,port=change_port):
                    configured=original
                    if change_address: configured=configured.replace('listen=0.0.0.0','listen=127.0.0.2')
                    if change_port: configured=configured.replace(f'port={app.port}',f'port={self.source.server.server_port}')
                    app.pref.write_text(configured)
                    if not automatic: self.assertEqual(get(app.origin+'/readconf')[0],200)
                    destination=app.origin+'/sub?target=clash'
                    path=f'/reload-redirect-{automatic}-{change_address}-{change_port}'
                    self.source.routes[path]=(302,b'',0,{'Location':destination})
                    for source in (destination,self.source.origin+path):
                        status,report,_=app.request(source,refresh='true')
                        self.assertGreaterEqual(status,400,report)
                        self.assertIn('Self-referencing',str(report))
                        self.assertEqual(report['downloads'][0]['attempts'],1)
                    self.assertEqual(self.source.counts[path],1)
                    status,raw,_=get(app.origin+'/status')
                    self.assertEqual(status,200)
                    state=json.loads(raw)
                    self.assertEqual(state['listen'],'127.0.0.2' if change_address else '0.0.0.0')
                    self.assertEqual(state['port'],self.source.server.server_port if change_port else app.port)
                    status,report,_=app.request(self.source.origin+'/sub',refresh='true')
                    self.assertEqual(status,200,report)
                    self.assertIn('fixture-node',report['output'])

    def test_self_request_rejected_at_each_redirect_hop(self):
        app=self.app(listen='0.0.0.0',parallel=2,fallback=False,workers=2)
        for resource in ('subscription','config','rules'):
            for code in (301,302,303,307,308):
                with self.subTest(resource=resource,code=code):
                    path=f'/redirect-{resource}-{code}'
                    source=self.source.origin+path
                    destination=app.origin.replace('127.0.0.1','127.0.0.2')+'/sub?'+urlencode({'target':'clash','url':source})
                    self.source.routes[path]=(code,b'',0,{'Location':destination})
                    config=None if resource=='subscription' else (source if resource=='config' else self.config([source]))
                    status,report,_=app.request(source if resource=='subscription' else self.source.origin+'/sub',config,refresh='true')
                    self.assertGreaterEqual(status,400,report)
                    self.assertIn('Self-referencing',str(report))
                    self.assertEqual(self.source.counts[path],1)
                    rejected=[d for d in report['downloads'] if 'Self-referencing' in d.get('error','')]
                    self.assertEqual(len(rejected),1,report); self.assertEqual(rejected[0]['attempts'],1)
                    self.assertEqual(get(app.origin+'/status')[0],200)
        self.source.routes['/first-hop']=(302,b'',0,{'Location':'/second-hop'})
        self.source.routes['/second-hop']=(307,b'',0,{'Location':app.origin+'/sub?target=clash'})
        status,report,_=app.request(self.source.origin+'/first-hop')
        self.assertGreaterEqual(status,400,report); self.assertIn('Self-referencing',str(report))
        self.assertEqual(self.source.counts['/first-hop'],1); self.assertEqual(self.source.counts['/second-hop'],1)

    def test_offline_generation_obeys_required_rule_policy(self):
        app=self.app()
        config=self.config([self.source.origin+'/rule'])
        self.source.routes['/rule']=(200,b'DOMAIN,old.example\n',0)
        items={'path':'offline.yml','target':'clash','url':self.source.origin+'/sub','config':config,'emoji':'false'}
        output=app.root/'offline.yml'
        warm=self.generate(app,[('fixture',items)])
        self.assertEqual(warm.returncode,0,warm.stderr); self.assertIn('old.example',output.read_text())
        counts=self.source.counts.copy()
        self.assertEqual(self.generate(app,[('fixture',items)]).returncode,0)
        self.assertEqual(self.source.counts,counts)
        self.expire(app)
        self.source.routes['/rule']=(403,b'denied',0)
        previous=output.read_bytes()
        before=self.source.counts['/rule']
        failed=self.generate(app,[('fixture',items)])
        self.assertNotEqual(failed.returncode,0,failed.stderr); self.assertIn('generate ERROR',failed.stderr)
        self.assertIn('Required ruleset',failed.stderr); self.assertEqual(output.read_bytes(),previous)
        self.assertEqual(self.source.counts['/rule'],before+1)
        before=self.source.counts['/rule']
        stale=self.generate(app,[('fixture',{**items,'use_stale':'true'})])
        self.assertEqual(stale.returncode,0,stale.stderr); self.assertIn('old.example',output.read_text())
        self.assertEqual(self.source.counts['/rule'],before)
        self.source.routes['/rule']=(200,b'DOMAIN,new.example\n',0)
        refreshed=self.generate(app,[('fixture',{**items,'refresh':'true','use_stale':'true'})])
        self.assertEqual(refreshed.returncode,0,refreshed.stderr); self.assertIn('new.example',output.read_text())
        self.assertNotIn('old.example',output.read_text()); self.assertEqual(self.source.counts['/rule'],before+1)
        self.source.routes['/rule']=(403,b'denied',0)
        previous=output.read_bytes()
        fallback=self.generate(app,[('fixture',{**items,'refresh':'true','use_stale':'true'})])
        self.assertEqual(fallback.returncode,0,fallback.stderr); self.assertEqual(output.read_bytes(),previous)
        self.assertEqual(self.source.counts['/rule'],before+2)
        forced=self.generate(app,[('fixture',{**items,'refresh':'true'})])
        self.assertNotEqual(forced.returncode,0,forced.stderr); self.assertEqual(output.read_bytes(),previous)
        self.assertEqual(self.source.counts['/rule'],before+3)
        mixed=self.generate(app,[('fixture',{**items,'refresh':'true'}),('plain',{'path':'plain.txt','target':'trojan','url':self.source.origin+'/sub'})])
        self.assertNotEqual(mixed.returncode,0,mixed.stderr); self.assertEqual(output.read_bytes(),previous)
        self.assertIn(b'trojan://',base64.b64decode((app.root/'plain.txt').read_text()))
        output.unlink()
        self.source.routes['/uncached-rule']=(403,b'denied',0)
        self.config([self.source.origin+'/uncached-rule'])
        cold=self.generate(app,[('fixture',{**items,'refresh':'true','use_stale':'true'})])
        self.assertNotEqual(cold.returncode,0,cold.stderr); self.assertFalse(output.exists())

    def test_offline_generation_handles_write_failures(self):
        app=self.app()
        for direct in (False,True):
            with self.subTest(direct=direct):
                items={'url':self.source.origin+'/sub'}
                items.update({'direct':'true'} if direct else {'target':'trojan'})
                bad={**items,'path':'missing/output.txt'}
                good={**items,'path':'complete.txt'}
                for sections in ([('bad',bad)],[('bad',bad),('good',good)]):
                    result=self.generate(app,sections)
                    self.assertNotEqual(result.returncode,0,result.stderr)
                    self.assertIn("Artifact 'bad' generate ERROR! Cannot write output file",result.stderr)
                    self.assertNotIn("Artifact 'bad' generate SUCCESS",result.stderr)
                    self.assertFalse((app.root/'missing').exists())
                    if len(sections)>1:
                        generated=(app.root/'complete.txt').read_bytes()
                        if direct: self.assertEqual(generated,b'\xef\xbb\xbf'+SUB)
                        else: self.assertIn(b'trojan://fixture-password@127.0.0.2:443',base64.b64decode(generated))
                destination=app.root/'occupied'
                destination.mkdir(exist_ok=True)
                sentinel=destination/'keep.txt'
                sentinel.write_bytes(b'keep old directory')
                result=self.generate(app,[('blocked',{**items,'path':'occupied'}),('good',good)])
                self.assertNotEqual(result.returncode,0,result.stderr)
                self.assertIn("Artifact 'blocked' generate ERROR! Cannot write output file",result.stderr)
                self.assertEqual(sentinel.read_bytes(),b'keep old directory')
                self.assertFalse(list(app.root.glob('*.tmp-*')))
                output=app.root/'complete.txt'
                output.write_bytes(b'previous artifact')
                if os.name=='posix': output.chmod(0o600)
                replaced=self.generate(app,[('good',good)])
                self.assertEqual(replaced.returncode,0,replaced.stderr)
                previous=output.read_bytes()
                if os.name=='posix':
                    self.assertEqual(output.stat().st_mode & 0o777,0o600)
                    result=self.generate(app,[('short',good)],file_size_limit=16)
                    self.assertNotEqual(result.returncode,0,result.stderr)
                    self.assertIn("Artifact 'short' generate ERROR! Cannot write output file",result.stderr)
                    self.assertEqual(output.read_bytes(),previous)
                    self.assertFalse(list(app.root.glob('*.tmp-*')))

    def test_offline_generation_preserves_symlinks(self):
        app=self.app()
        (app.root/'outputs').mkdir()
        (app.root/'links').mkdir()
        output=app.root/'outputs'/'target.txt'
        intermediate=app.root/'links'/'next.txt'
        published=app.root/'published.txt'
        try:
            intermediate.symlink_to('../outputs/target.txt')
            published.symlink_to('links/next.txt')
        except OSError as error:
            if os.name=='nt' and error.winerror==1314: self.skipTest('runner cannot create symbolic links')
            raise
        for direct in (False,True):
            with self.subTest(direct=direct):
                items={'path':'published.txt','url':self.source.origin+'/sub'}
                items.update({'direct':'true'} if direct else {'target':'trojan'})
                output.write_bytes(b'previous artifact')
                if os.name=='posix': output.chmod(0o600)
                generated=self.generate(app,[('linked',items)])
                self.assertEqual(generated.returncode,0,generated.stderr)
                self.assertTrue(published.is_symlink()); self.assertTrue(intermediate.is_symlink())
                self.assertEqual(Path(os.readlink(published)),Path('links/next.txt'))
                self.assertEqual(Path(os.readlink(intermediate)),Path('../outputs/target.txt'))
                content=output.read_bytes()
                if direct: self.assertEqual(content,b'\xef\xbb\xbf'+SUB)
                else: self.assertIn(b'trojan://fixture-password@127.0.0.2:443',base64.b64decode(content))
                if os.name=='posix':
                    self.assertEqual(output.stat().st_mode & 0o777,0o600)
                    failed=self.generate(app,[('linked',items)],file_size_limit=16)
                    self.assertNotEqual(failed.returncode,0,failed.stderr)
                    self.assertEqual(output.read_bytes(),content)
                    self.assertTrue(published.is_symlink()); self.assertTrue(intermediate.is_symlink())
                    self.assertFalse(list((app.root/'outputs').glob('*.tmp-*')))
                output.unlink()
                dangling=self.generate(app,[('linked',items)])
                self.assertEqual(dangling.returncode,0,dangling.stderr)
                self.assertEqual(output.read_bytes(),content)
                self.assertTrue(published.is_symlink()); self.assertTrue(intermediate.is_symlink())
        published.unlink()
        published.symlink_to(output)
        absolute=self.generate(app,[('linked',items)])
        self.assertEqual(absolute.returncode,0,absolute.stderr)
        self.assertTrue(published.is_symlink()); self.assertEqual(published.read_bytes(),output.read_bytes())
        unicode_output=app.root/'outputs'/'目标-雪.txt'
        for target in ('outputs/目标-雪.txt',unicode_output):
            published.unlink()
            unicode_output.write_bytes(b'previous artifact')
            published.symlink_to(target)
            generated=self.generate(app,[('linked',items)])
            self.assertEqual(generated.returncode,0,generated.stderr)
            self.assertTrue(published.is_symlink()); self.assertEqual(unicode_output.read_bytes(),content)
        published.unlink(); intermediate.unlink()
        published.symlink_to('links/next.txt'); intermediate.symlink_to('../published.txt')
        cyclic=self.generate(app,[('linked',items)])
        self.assertNotEqual(cyclic.returncode,0,cyclic.stderr)
        self.assertIn('Cannot write output file',cyclic.stderr)
        self.assertTrue(published.is_symlink()); self.assertTrue(intermediate.is_symlink())

    def test_offline_generation_rejects_hard_linked_targets(self):
        app=self.app()
        output=app.root/'private.txt'
        alias=app.root/'published.txt'
        link=app.root/'linked.txt'
        output.write_bytes(b'previous artifact')
        output.chmod(0o600)
        os.link(output,alias)
        link.symlink_to('private.txt')
        before=output.stat()
        self.assertEqual(before.st_nlink,2)
        for direct in (False,True):
            for linked in (False,True):
                with self.subTest(direct=direct,linked=linked):
                    items={'path':'linked.txt' if linked else 'private.txt','url':self.source.origin+'/sub'}
                    items.update({'direct':'true'} if direct else {'target':'trojan'})
                    good={**items,'path':'complete.txt'}
                    for sections in ([('linked',items)],[('linked',items),('good',good)]):
                        result=self.generate(app,sections)
                        self.assertNotEqual(result.returncode,0,result.stderr)
                        self.assertIn("Artifact 'linked' generate ERROR! Cannot write output file",result.stderr)
                        self.assertNotIn("Artifact 'linked' generate SUCCESS",result.stderr)
                        for path in (output,alias):
                            self.assertEqual(path.read_bytes(),b'previous artifact')
                            after=path.stat()
                            self.assertEqual((after.st_dev,after.st_ino,after.st_nlink,after.st_mode),
                                             (before.st_dev,before.st_ino,before.st_nlink,before.st_mode))
                        self.assertTrue(link.is_symlink())
                        self.assertFalse(list(app.root.glob('*.tmp-*')))
                        if len(sections)>1:
                            generated=(app.root/'complete.txt').read_bytes()
                            if direct: self.assertEqual(generated,b'\xef\xbb\xbf'+SUB)
                            else: self.assertIn(b'trojan://fixture-password@127.0.0.2:443',base64.b64decode(generated))
        alias.unlink()
        recovered=self.generate(app,[('linked',items)])
        self.assertEqual(recovered.returncode,0,recovered.stderr)
        self.assertEqual(output.read_bytes(),b'\xef\xbb\xbf'+SUB)
        self.assertTrue(link.is_symlink())

    @unittest.skipIf(os.name=='nt','POSIX ownership and ACL regression')
    def test_offline_generation_preserves_extended_permissions(self):
        app=self.app()
        output=app.root/'private.txt'
        link=app.root/'published.txt'
        link.symlink_to('private.txt')
        for direct in (False,True):
            for linked in (False,True):
                with self.subTest(direct=direct,linked=linked):
                    output.write_bytes(b'previous artifact')
                    if os.geteuid()==0: os.chown(output,65534,65534)
                    output.chmod(0o2640)
                    if sys.platform.startswith('linux'):
                        acl=struct.pack('<I',2)+b''.join(struct.pack('<HHI',tag,permission,identifier) for tag,permission,identifier in (
                            (1,6,0xffffffff),(2,4,12345),(4,0,0xffffffff),(16,4,0xffffffff),(32,0,0xffffffff)))
                        os.setxattr(output,'system.posix_acl_access',acl)
                        permissions=os.getxattr(output,'system.posix_acl_access')
                    elif sys.platform=='darwin':
                        subprocess.run(['chmod','-N',str(output)],check=True,capture_output=True)
                        subprocess.run(['chmod','+a','everyone allow read',str(output)],check=True,capture_output=True)
                        permissions=subprocess.run(['ls','-le',str(output)],check=True,capture_output=True,text=True).stdout.splitlines()[1:]
                    before=output.stat()
                    items={'path':'published.txt' if linked else 'private.txt','url':self.source.origin+'/sub'}
                    items.update({'direct':'true'} if direct else {'target':'trojan'})
                    result=self.generate(app,[('private',items)])
                    self.assertEqual(result.returncode,0,result.stderr)
                    after=output.stat()
                    self.assertEqual((after.st_uid,after.st_gid,after.st_mode & 0o7777),(before.st_uid,before.st_gid,before.st_mode & 0o7777))
                    if sys.platform.startswith('linux'): self.assertEqual(os.getxattr(output,'system.posix_acl_access'),permissions)
                    elif sys.platform=='darwin': self.assertEqual(subprocess.run(['ls','-le',str(output)],check=True,capture_output=True,text=True).stdout.splitlines()[1:],permissions)
                    self.assertTrue(link.is_symlink())
                    generated=output.read_bytes()
                    if direct: self.assertEqual(generated,b'\xef\xbb\xbf'+SUB)
                    else: self.assertIn(b'trojan://fixture-password@127.0.0.2:443',base64.b64decode(generated))
                    if sys.platform=='darwin':
                        subprocess.run(['chmod','-N',str(output)],check=True,capture_output=True)
                        no_acl=self.generate(app,[('private',items)])
                        self.assertEqual(no_acl.returncode,0,no_acl.stderr)
                        self.assertEqual(subprocess.run(['ls','-le',str(output)],check=True,capture_output=True,text=True).stdout.splitlines()[1:],[])

    def test_configuration_format_parity(self):
        app=self.app()
        configs={
            'ini':'[common]\nproxy_subscription=NONE\n[server]\nlisten=127.0.0.1\nport=25500\n[advanced]\nmax_parallel_downloads=2\ndownload_timeout=7\nconnect_timeout=3\nsubscription_source_headers={}\n',
            'toml':'version = 1\n[common]\nproxy_subscription="NONE"\n[server]\nlisten="127.0.0.1"\nport=25500\n[advanced]\nmax_parallel_downloads=2\ndownload_timeout=7\nconnect_timeout=3\nsubscription_source_headers="{}"\n',
            'yml':'common:\n  proxy_subscription: NONE\nserver:\n  listen: 127.0.0.1\n  port: 25500\nadvanced:\n  max_parallel_downloads: 2\n  download_timeout: 7\n  connect_timeout: 3\n  subscription_source_headers: "{}"\n'
        }
        for ext,body in configs.items():
            path=app.root/('parity.'+ext);path.write_text(body)
            check=subprocess.run([str(BINARY),'-f',str(path),'--check'],capture_output=True,text=True,timeout=10)
            self.assertEqual(check.returncode,0,check.stderr)
            data=json.loads(check.stdout[check.stdout.index('{'):])
            self.assertEqual(data['max_parallel_downloads'],2)
            self.assertEqual(data['download_timeout'],7)
            self.assertEqual(data['connect_timeout'],3)
    def test_static_conversion_entry(self):
        app=self.app()
        status,body,_=get(app.origin+'/')
        self.assertEqual(status,200)
        self.assertIn(b'converter.js',body)
        self.assertEqual(get(app.origin+'/converter.js')[0],200)

if __name__=='__main__':
    parser=argparse.ArgumentParser()
    parser.add_argument('--binary',required=True,type=Path)
    parser.add_argument('--base',type=Path,default=Path(__file__).resolve().parents[1]/'base')
    args,remaining=parser.parse_known_args()
    BINARY=args.binary.resolve();BASE=args.base.resolve()
    unittest.main(argv=['integration.py',*remaining])
