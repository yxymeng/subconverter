#!/usr/bin/env python3
"""Cold/warm/expiry/force comparisons with deterministic local source latency.
An optional public rules repository supplies actual rule bodies without private subscriptions.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import statistics
import time
import threading
import subprocess
from urllib.parse import urlsplit
import integration

parser=argparse.ArgumentParser()
parser.add_argument('--binary',required=True,type=Path)
parser.add_argument('--base',type=Path,default=Path(__file__).resolve().parents[1]/'base')
parser.add_argument('--rules-repo',type=Path)
parser.add_argument('--repeats',type=int,default=3)
parser.add_argument('--output',type=Path)
args=parser.parse_args()
integration.BINARY=args.binary.resolve();integration.BASE=args.base.resolve()
def usage(pid):
    try:
        stat=Path(f'/proc/{pid}/stat').read_text().split()
        status=dict(line.split(':',1) for line in Path(f'/proc/{pid}/status').read_text().splitlines() if ':' in line)
        return ((int(stat[13])+int(stat[14]))/os.sysconf('SC_CLK_TCK'),int(status['VmRSS'].split()[0]),int(status['Threads']))
    except (OSError,ValueError,KeyError):return None

source=integration.Source()
config=[];corpus=[]
if args.rules_repo:
    ini=(args.rules_repo/'Ruleset/Clash.ini').read_text()
    for group,url in re.findall(r'^ruleset=([^,]+),(.+)$',ini,re.M):
        if url.startswith('[]'): config.append((group,url));continue
        remote_path=urlsplit(url).path
        relative=remote_path[remote_path.index('/Ruleset/')+1:] if 'yxymeng/yxymeng.github.io/' in remote_path and '/Ruleset/' in remote_path else '__external__'
        file=args.rules_repo/relative
        if file.is_file(): body=file.read_bytes();kind='actual public corpus'
        else: body=b'DOMAIN-SUFFIX,fixture.example\n';kind='external source substituted'
        path='/rule-'+str(len(corpus));source.routes[path]=(200,body,.05)
        config.append((group,source.origin+path));corpus.append({'url':url,'bytes':len(body),'kind':kind,'sha256':hashlib.sha256(body).hexdigest()})
else:
    for i in range(25):
        body=('DOMAIN-SUFFIX,rule-%d.example\n'%i).encode();path='/rule-'+str(i)
        source.routes[path]=(200,body,.05);config.append(('DIRECT',source.origin+path))
    config.append(('DIRECT','[]MATCH'))
# Keep original rule order and groups while providing all group names as local direct groups.
groups=list(dict.fromkeys(g for g,u in config if g not in ['DIRECT','REJECT']))
external='[custom]\nenable_rule_generator=true\noverwrite_original_rules=true\n'
external+=''.join('custom_proxy_group='+g+'`select`[]DIRECT\n' for g in groups)
external+=''.join('ruleset='+g+','+u+'\n' for g,u in config)
source.routes['/benchmark-config']=(200,external.encode(),0)
config_url=source.origin+'/benchmark-config'
results=[]
try:
    for async_value in [False,True]:
        for repetition in range(args.repeats):
            app=integration.App(asynchronous=async_value,parallel=4)
            try:
                for state in ['cold','valid','expired','forced']:
                    if state=='expired':
                        for path in (app.root/'cache').glob('v2-*'):os.utime(path,(time.time()-120,time.time()-120))
                    before=sum(source.counts.values())
                    samples=[];stop=threading.Event();initial=usage(app.process.pid)
                    def sample():
                        while not stop.wait(.01):
                            value=usage(app.process.pid)
                            if value:samples.append(value)
                    sampler=threading.Thread(target=sample);sampler.start()
                    start=time.perf_counter()
                    code,report,_=app.request(source.origin+'/sub',config_url,**({'refresh':'true'} if state=='forced' else {}))
                    elapsed=(time.perf_counter()-start)*1000
                    final=usage(app.process.pid);stop.set();sampler.join()
                    if code!=200:raise RuntimeError(report)
                    results.append({'async':async_value,'repetition':repetition,'state':state,'wall_ms':elapsed,'metrics':report.get('metrics',{}),
                        'cpu_ms':(final[0]-initial[0])*1000 if initial and final else None,
                        'peak_rss_kib':max((v[1] for v in samples),default=None),'peak_threads':max((v[2] for v in samples),default=None),
                        'phases':report['phases'],'network_requests':sum(source.counts.values())-before,
                        'network_bytes':sum(d['bytes'] for d in report['downloads'] if d['cache']=='network'),
                        'output_bytes':len(report['output'].encode()),'output_sha256':hashlib.sha256(report['output'].encode()).hexdigest()})
            finally:app.close()
finally:source.close()
hashes={r['output_sha256'] for r in results}
if len(hashes)!=1:raise RuntimeError('Output changed between parallelism/cache states')
report={'environment':'Linux; local HTTP, 50ms fixed latency per ruleset; one synthetic Trojan node',
        'corpus':corpus,'rules_repo_commit':subprocess.check_output(['git','-C',str(args.rules_repo),'rev-parse','HEAD'],text=True).strip() if args.rules_repo else None,'output_identical':True,'runs':results,'medians':[]}
for mode in [False,True]:
    for state in ['cold','valid','expired','forced']:
        rows=[r for r in results if r['async']==mode and r['state']==state]
        report['medians'].append({'async':mode,'state':state,'wall_ms':statistics.median(r['wall_ms'] for r in rows),
            'network_requests':statistics.median(r['network_requests'] for r in rows),'network_bytes':statistics.median(r['network_bytes'] for r in rows),
            'cpu_ms':statistics.median(r['cpu_ms'] for r in rows) if all(r['cpu_ms'] is not None for r in rows) else None,
            'peak_rss_kib':max(r['peak_rss_kib'] for r in rows) if all(r['peak_rss_kib'] is not None for r in rows) else None,
            'peak_threads':max(r['peak_threads'] for r in rows) if all(r['peak_threads'] is not None for r in rows) else None,
            'phases_ms':{key:statistics.median(sum(p['duration_ms'] for p in r['phases'] if p['phase']==key) for r in rows) for key in ['rules_download','rules_processing','export','total']}})
if args.output:args.output.write_text(json.dumps(report,indent=2,ensure_ascii=False)+'\n')
print(json.dumps(report['medians'],indent=2))
