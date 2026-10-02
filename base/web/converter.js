(function (root) {
  'use strict';
  const known = ['target','url','config','include','exclude','filename','emoji','udp','sort','refresh','use_stale','token'];
  function build(origin, values, extra = {}) {
    const url = new URL('/sub', origin);
    const params = new URLSearchParams(extra);
    for (const key of known) {
      params.delete(key);
      if (values[key] !== undefined && values[key] !== '') params.set(key, String(values[key]));
    }
    // One subscription per line, preserving URL query strings and inline node links.
    const sources = (values.url || '').split(/\r?\n/).map(s => s.trim()).filter(Boolean).join('|');
    if (sources) params.set('url', sources);
    else params.delete('url');
    if (!params.get('target')) throw new Error('请选择目标格式。');
    url.search = params.toString();
    return url.toString();
  }
  function restore(text) {
    const url = new URL(text);
    if (!['http:','https:'].includes(url.protocol) || !['/sub','/diagnose'].includes(url.pathname)) throw new Error('请输入完整的 /sub 转换链接。');
    const values = {}, extra = {};
    for (const [key,value] of url.searchParams) (known.includes(key) ? values : extra)[key] = value;
    if (values.url) values.url = values.url.split('|').join('\n');
    return {values, extra};
  }
  const api = {build, restore};
  if (typeof module !== 'undefined' && module.exports) module.exports = api;
  else root.Subconverter = api;
})(typeof window !== 'undefined' ? window : globalThis);
