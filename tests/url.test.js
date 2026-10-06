'use strict';
const assert = require('node:assert/strict');
const api = require('../base/web/converter.js');
const original = 'https://old.example/sub?' + new URLSearchParams({target:'clash',url:'https://a.example/sub?token=x%2Fy&flag=1|trojan://p@host:443#香港',config:'https://c.example/规则.ini?x=1&y=2',include:'港|台',emoji:'false',new_name:'true',interval:'86400'});
const restored = api.restore(original);
const rebuilt = new URL(api.build('http://192.168.1.10:25500',restored.values,restored.extra));
assert.equal(rebuilt.origin,'http://192.168.1.10:25500');
assert.deepEqual([...rebuilt.searchParams].sort(),[...new URL(original).searchParams].sort());
assert.equal(api.restore(rebuilt.toString()).values.url,restored.values.url);
assert.equal(new URL(api.build('http://localhost', {target:'clash',url:' \n '})).searchParams.has('url'),false);
const defaultSubscription=api.restore('http://localhost/sub?target=clash');
assert.equal(new URL(api.build('http://localhost',defaultSubscription.values,defaultSubscription.extra)).search,'?target=clash');
assert.throws(()=>api.restore('javascript:alert(1)'));
assert.throws(()=>api.restore('https://example.com/unrelated'));
const fs = require('node:fs');
const vm = require('node:vm');
const ids = ['target','url','config','include','exclude','filename','emoji','udp','sort','refresh','use_stale','token','preset','form','generate','restore','existing','link','copy','download','output','size','phases','downloads','warnings','request-id','message','config-field'];
const elements = Object.fromEntries(ids.map(id => [id, {
  value:'', type:['refresh','use_stale'].includes(id) ? 'checkbox' : 'text',
  checked:false, listeners:{}, addEventListener(event, handler) { this.listeners[event]=handler; },
  replaceChildren() {}
}]));
const context = vm.createContext({
  document:{getElementById:id=>elements[id]}, location:{origin:'http://localhost'},
  Subconverter:api, fetch:()=>new Promise(()=>{})
});
vm.runInContext(fs.readFileSync(require.resolve('../base/web/app.js'),'utf8'),context);
for (const query of [
  'include=+fixture-node+&exclude=+other+',
  'include=fixture-node&include=definitely-not-a-node&new_name=true&new_name=false',
  'include=&include=fixture-node&interval=&interval=86400'
]) {
  const input = new URL('http://old.example/sub?target=clash&'+query);
  elements.existing.value=input.href;
  elements.restore.listeners.click();
  const output = new URL(elements.link.value);
  for (const key of new Set(input.searchParams.keys())) {
    assert.equal(output.searchParams.get(key) || '',input.searchParams.get(key) || '',key+' must retain the backend first value');
  }
}
console.log('URL encoding, restoration and same-origin cases passed');
