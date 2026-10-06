'use strict';
const $ = id => document.getElementById(id);
const fields = ['target','url','config','include','exclude','filename','emoji','udp','sort','refresh','use_stale','token'];
const phaseNames = {total:'总耗时',external_config:'外部配置',rules_download:'规则下载',subscription_download:'订阅下载',subscription_parse:'订阅解析',rules_processing:'规则处理',export:'导出'};
let extras = {}, result = '', controller;
function message(text, style='') { $('message').textContent=text; $('message').className='message '+style; }
function values() {
  const data={};
  for(const key of fields) { const el=$(key); data[key]=el.type==='checkbox' ? String(el.checked) : ['include','exclude'].includes(key) ? el.value : el.value.trim(); }
  if($('preset').value==='default') data.config='';
  return data;
}
function generate() {
  const link=Subconverter.build(location.origin,values(),extras);
  $('link').value=link; $('copy').disabled=false;
  return link;
}
function resetPreview() {
  controller?.abort();
  result='';$('download').disabled=true;$('output').textContent='转换后的配置会显示在这里。';$('size').textContent='尚未转换';
  $('phases').replaceChildren();$('downloads').replaceChildren();$('warnings').textContent='';$('request-id').textContent='';
}
$('form').addEventListener('input',()=>{resetPreview();message('参数已变更，请重新生成链接或转换。');});
$('preset').addEventListener('change',()=>{
  $('config-field').hidden=$('preset').value==='default';
  if($('preset').value==='yxymeng') $('config').value='https://yxymeng.github.io/Ruleset/Clash.ini';
  else if($('preset').value==='default') $('config').value='';
});
$('generate').addEventListener('click',()=>{
  try { generate();resetPreview();message('转换链接已生成；尚未向后端验证。'); }
  catch(error) { message(error.message,'error'); }
});
$('restore').addEventListener('click',()=>{
  try {
    const data=Subconverter.restore($('existing').value.trim());extras=data.extra;
    for(const key of fields) {
      const el=$(key), value=data.values[key]||'';
      if(el.type==='checkbox')el.checked=value==='true';
      else {
        el.value=value;
        if(el.tagName==='SELECT' && value && el.value!==value) {
          const opt=document.createElement('option');opt.value=value;opt.textContent='还原参数：'+value;el.append(opt);el.value=value;
        }
      }
    }
    $('target').value=data.values.target||'clash';
    $('preset').value=data.values.config?'custom':'default';$('config-field').hidden=!data.values.config;
    generate();resetPreview();message('参数已还原，转换链接使用当前服务地址。');
  } catch(error) { message(error.message,'error'); }
});
$('form').addEventListener('submit',async event=>{
  event.preventDefault(); if(controller)return;
  try {
    const link=generate();resetPreview();
    controller=new AbortController();$('preview').disabled=true;
    const pending=controller;
    const start=performance.now();
    message('正在转换，等待订阅和规则加载完成…');
    const timer=setInterval(()=>{if(!pending.signal.aborted)message(`正在转换… 已等待 ${((performance.now()-start)/1000).toFixed(1)} 秒`);},500);
    try {
      const request=new URL(link);request.pathname='/diagnose';
      const response=await fetch(request,{signal:pending.signal,cache:'no-store'});
      const report=await response.json();
      if(pending.signal.aborted)return;
      $('diagnostics').open=true;$('request-id').textContent=report.request_id||'';
      for(const phase of report.phases||[]) {
        const tile=document.createElement('div');tile.className='metric';tile.textContent=phaseNames[phase.phase]||phase.phase;
        const value=document.createElement('strong');value.textContent=phase.duration_ms.toFixed(0)+' ms';tile.append(value);$('phases').append(tile);
      }
      for(const entry of report.downloads||[]) {
        const row=document.createElement('tr');
        for(const value of [entry.source+' · '+entry.source_id.slice(0,6),entry.error||String(entry.http_status),entry.cache,entry.duration_ms.toFixed(0)+' ms']) {const cell=document.createElement('td');cell.textContent=value;row.append(cell);}
        $('downloads').append(row);
      }
      $('warnings').textContent=(report.warnings||[]).join('\n');
      if(!response.ok || !report.success) throw new Error(report.error||'后端转换失败，请查看诊断。');
      result=report.output;$('output').textContent=result;$('download').disabled=false;
      $('size').textContent=new TextEncoder().encode(result).length.toLocaleString()+' 字节';
      message('后端转换成功，已载入实际结果。'+(report.warnings?.length?' 部分订阅被跳过，请查看诊断。':''),'success');
    } finally {clearInterval(timer);}
  } catch(error) { if(error.name!=='AbortError')message(error.message,'error'); }
  finally {controller=undefined;$('preview').disabled=false;}
});
$('copy').addEventListener('click',async()=>{
  try {
    if(navigator.clipboard&&window.isSecureContext) await navigator.clipboard.writeText($('link').value);
    else {$('link').focus();$('link').select();if(!document.execCommand('copy'))throw new Error('请选中转换链接后手动复制。');}
    message('转换链接已复制。');
  } catch(error) {message(error.message,'error');}
});
$('download').addEventListener('click',()=>{
  const url=URL.createObjectURL(new Blob([result],{type:'text/plain;charset=utf-8'}));
  const anchor=document.createElement('a');anchor.href=url;anchor.download=$('filename').value||($('target').value==='singbox'?'config.json':'config.yaml');anchor.click();setTimeout(()=>URL.revokeObjectURL(url),1000);
});
fetch('/status',{cache:'no-store'}).then(r=>{if(!r.ok)throw new Error();return r.json();}).then(data=>{
  $('service').textContent='服务已连接 · '+location.host;$('version').textContent=data.version+' · '+data.build_commit;
}).catch(()=>{$('service').textContent='服务未连接';message('无法读取服务状态，请检查后台是否正在运行。','error');});
