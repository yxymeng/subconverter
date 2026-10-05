#!/usr/bin/env python3
"""Optional real Chromium acceptance for the same-origin conversion page."""
import argparse
from pathlib import Path
from playwright.sync_api import sync_playwright
import integration
import time

parser=argparse.ArgumentParser()
parser.add_argument('--binary',required=True,type=Path)
parser.add_argument('--chromium',default='/usr/bin/chromium')
parser.add_argument('--screenshots',type=Path)
args=parser.parse_args()
integration.BINARY=args.binary.resolve();integration.BASE=Path(__file__).resolve().parents[1]/'base'
source=integration.Source();app=integration.App(default_url=source.origin+'/sub');errors=[]
try:
    with sync_playwright() as p:
        browser=p.chromium.launch(executable_path=args.chromium,headless=True,args=['--no-sandbox'])
        page=browser.new_page(viewport={'width':1440,'height':1100})
        page.on('pageerror',lambda error: errors.append(str(error)))
        page.goto(app.origin)
        page.locator('#service').filter(has_text='服务已连接').wait_for()
        page.locator('#url').fill(source.origin+'/sub?token=synthetic-test-token')
        page.locator('#generate').click()
        assert '尚未向后端验证' in page.locator('#message').inner_text()
        assert page.locator('#link').input_value().startswith(app.origin+'/sub?')
        page.locator('#preview').click()
        page.locator('#message.success').wait_for()
        assert 'fixture-node' in page.locator('#output').inner_text()
        assert 'subscription_download' not in page.locator('#phases').inner_text() # localized phase names
        assert page.locator('#downloads tr').count()==1
        assert page.locator('#download').is_enabled()
        with page.expect_download() as download:
            page.locator('#download').click()
        assert 'fixture-node' in Path(download.value.path()).read_text()
        page.locator('#copy').click()
        page.locator('#message').filter(has_text='已复制').wait_for()
        if args.screenshots:
            args.screenshots.mkdir(parents=True,exist_ok=True)
            page.screenshot(path=str(args.screenshots/'web-desktop.png'),full_page=True)
        # Advanced parameters and unsupported target survive restoration, URL values aren't double encoded.
        old=app.origin+'/sub?target=trojan&url='+source.origin+'%2Fsub%3Ftoken%3Dx%252Fy&new_name=true'
        page.locator('.import summary').click();page.locator('#existing').fill(old);page.locator('#restore').click()
        assert page.locator('#target').input_value()=='trojan'
        assert 'new_name=true' in page.locator('#link').input_value()
        assert 'udp=' not in page.locator('#link').input_value()
        assert 'emoji=' not in page.locator('#link').input_value()
        # Defaults remain valid when an existing link omits the subscription URL.
        page.locator('#existing').fill(app.origin+'/sub?target=clash');page.locator('#restore').click()
        assert '参数已还原' in page.locator('#message').inner_text()
        assert 'url=' not in page.locator('#link').input_value()
        page.locator('#preview').click();page.locator('#message.success').wait_for()
        assert 'fixture-node' in page.locator('#output').inner_text()
        # An old response cannot validate a link generated after its parameters change.
        source.routes['/slow']=(200,integration.SUB,.8)
        page.locator('#url').fill(source.origin+'/slow');page.locator('#preview').click()
        page.wait_for_function("document.getElementById('preview').disabled")
        page.locator('#url').fill(source.origin+'/changed');page.locator('#generate').click()
        page.wait_for_function("!document.getElementById('preview').disabled")
        time.sleep(1)
        assert '尚未向后端验证' in page.locator('#message').inner_text()
        assert not page.locator('#download').is_enabled()
        assert 'fixture-node' not in page.locator('#output').inner_text()
        assert '/changed' in page.locator('#link').input_value().replace('%2F','/')
        page.locator('#preview').click();page.locator('#message.success').wait_for()
        assert 'fixture-node' in page.locator('#output').inner_text()
        # A required rule failure is shown with a manual stale choice; no silent partial result.
        source.routes['/rule']=(403,b'denied',0)
        source.routes['/config']=(200,('[custom]\nruleset=DIRECT,'+source.origin+'/rule\nenable_rule_generator=true\n').encode(),0)
        page.locator('#target').select_option('clash');page.locator('#url').fill(source.origin+'/sub')
        page.locator('#preset').select_option('custom');page.locator('#config').fill(source.origin+'/config')
        page.locator('#preview').click();page.locator('#message.error').wait_for()
        assert 'Required ruleset' in page.locator('#message').inner_text()
        assert not page.locator('#download').is_enabled()
        page.set_viewport_size({'width':390,'height':844})
        assert page.evaluate('document.documentElement.scrollWidth <= innerWidth')
        if args.screenshots:page.screenshot(path=str(args.screenshots/'web-mobile.png'),full_page=True)
        assert not errors,errors
        browser.close()
    print('Chromium: real conversion, diagnostics, URL restoration, copying, download and mobile layout passed')
finally:app.close();source.close()
