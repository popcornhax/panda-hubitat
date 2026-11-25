#pragma once

namespace esphome::dashboard_config {
static constexpr char WEB_UI[] = R"HTML(<!doctype html><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>Dashboard layout</title>
<style>body{font:16px system-ui;background:#0b1220;color:#f1f5f9;margin:20px}main{max-width:1100px;margin:auto}section,.card{background:#141f30;border-radius:12px;padding:16px;margin:12px 0}input,select,button{font:inherit;padding:8px;margin:4px;border-radius:6px;border:1px solid #536070}input[type=color]{width:56px;height:40px;padding:3px;vertical-align:middle}input,select{background:#0b1220;color:inherit}button{background:#49d7c8;color:#08131d;cursor:pointer}.slotgrid{display:grid;grid-template-columns:repeat(3,minmax(0,1fr));gap:10px;margin-top:12px}.slotgrid select{width:100%}.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(260px,1fr));gap:10px}.page{border:1px solid #536070;padding:12px;margin:8px 0}.row{display:flex;flex-wrap:wrap;align-items:center}.preview{min-height:120px;border-radius:12px;padding:16px;background:#0b1220}.small{opacity:.75}#msg{font-weight:600}</style>
<main><h1>Dashboard layout</h1><p class=small>Changes are saved on this device and applied after a restart. <a href="/api/screen.bmp" target="_blank">Capture the current panel screen</a>.</p>
<section><h2>Devices</h2><p class=small>Add devices to the Panda Touch Maker API first. Scan and choose supported devices, then assign up to six per page. Up to 32 devices.</p><button id=scan>Scan Maker API</button><span id=scanStatus></span><div id=deviceChoices class=grid></div><button id=applyDevices>Use selected devices</button><span id=selectedCount></span></section>
<section><h2>Theme</h2><div class=grid><label>Background <input type=color id=background></label><label>Cards <input type=color id=panel></label><label>Text <input type=color id=text></label><label>Secondary text <input type=color id=muted></label><label>Accent <input type=color id=accent></label><label>Active / on <input type=color id=active></label><label>Updating <input type=color id=updating></label><label>Warning / offline <input type=color id=warning></label><label>Weather / info <input type=color id=info></label><label>Font size <select id=font><option value=normal>Normal</option><option value=large>Large</option></select></label></div></section>
<section><h2>Pages and cards</h2><p id=pageCount></p><p class=small>Choose available devices. A device can appear on multiple pages. Reorder pages and selected cards with the arrows.</p><div id=pages></div><button id=addPage>Add page</button></section>
<section><h2>Device labels</h2><div id=labels class=grid></div></section>
<section><h2>Screen sleep</h2><label><input type=checkbox id=sleepEnabled>Enable automatic screen sleep</label><div class=grid><label>Wake sensor <select id=wakeSensor></select></label><label>No motion or touch for (seconds) <input id=sleepSeconds type=number min=30 max=3600 step=1 value=120></label><label>Fade on (milliseconds) <input id=fadeMs type=number min=0 max=3000 step=100 value=800></label></div><p class=small>The backlight turns off while the controller stays online. Motion or a first touch wakes it. Sleep pauses if the sensor is offline or stale. The wake sensor must be selected above, but does not need a card.</p><button id=wakeNow>Wake screen now</button><button id=refreshStatus>Refresh status</button><p id=sleepStatus></p></section>
<section><h2>Background image</h2><input type=file id=image accept="image/*"><button id=uploadImage>Stage image for Save and restart</button><button id=removeImage>Remove image on Save</button><p class=small>Image changes apply with your other settings when you press Save and restart.</p><canvas id=canvas width=400 height=240 style="max-width:100%"></canvas></section>

<section><button id=save>Save and restart</button> <button id=restore>Restore defaults</button> <button id=download>Download JSON</button> <label>Upload JSON <input type=file id=upload accept="application/json,.json"></label><span id=msg></span></section></main>
<script>
const $=id=>document.getElementById(id),colors=['background','panel','text','muted','accent','active','updating','warning','info'];
let cfg,devices=[],catalogue=[],provisionDirty=false,busy=false,imageReady=false,stagedBackground=null,removeBackground=false;
const canvas=$('canvas'),ctx=canvas.getContext('2d');
function hex(n){return '#'+Number(n).toString(16).padStart(6,'0')}
function num(s){return parseInt(s.slice(1),16)}
function esc(s){return String(s).replace(/[&<>"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]))}
function sleepDefaults(){return {enabled:false,motion_device_id:0,timeout_seconds:120,fade_ms:800}}
function collect(){
 for(const c of colors)cfg.theme[c]=num($(c).value);cfg.font=$('font').value;
 cfg.pages.forEach((p,i)=>{p.label=$('nav'+i).value;p.title=$('title'+i).value;
  const picked=[...document.querySelectorAll(`.slot${i}`)].map(x=>Number(x.value)).filter(id=>id>0);if(new Set(picked).size!==picked.length)throw Error('Each device can occupy one slot per page');p.devices=picked});
 cfg.labels=devices.map(d=>({id:d.id,label:$('label'+d.id).value})).filter(x=>x.label.trim()&&x.label!==devices.find(d=>d.id===x.id).label);
 cfg.sleep={enabled:$('sleepEnabled').checked,motion_device_id:Number($('wakeSensor').value),
  timeout_seconds:Number($('sleepSeconds').value),fade_ms:Number($('fadeMs').value)};

}
function renderSleep(){cfg.sleep??=sleepDefaults();$('sleepEnabled').checked=cfg.sleep.enabled;
 $('wakeSensor').innerHTML='<option value="0">Choose a motion sensor</option>'+devices.filter(d=>d.motion_capable||d.kind==='motion').map(d=>`<option value=${d.id}>${esc(d.label)}</option>`).join('');
 $('wakeSensor').value=String(cfg.sleep.motion_device_id);if(!$('wakeSensor').value)$('wakeSensor').value='0';
 $('sleepSeconds').value=cfg.sleep.timeout_seconds;$('fadeMs').value=cfg.sleep.fade_ms;
}
function renderSelection(){const rows=catalogue.length?catalogue:devices.map(d=>({...d,supported:true}));
 $('deviceChoices').innerHTML=rows.map(d=>`<label class=card><input class=catalogPick type=checkbox value=${d.id} ${d.supported&&devices.some(x=>x.id===d.id)?'checked':''} ${d.supported?'':'disabled'}> ${esc(d.label)} <span class=small>(${esc(d.kind)}) ${esc(d.reason||'')}</span></label>`).join('');
 $('selectedCount').textContent=devices.length+' / 32 selected';
}
function render(){for(const c of colors)$(c).value=hex(cfg.theme[c]);$('font').value=cfg.font;$('pages').innerHTML='';
 cfg.pages.forEach((p,i)=>{const div=document.createElement('div');div.className='page';
 div.innerHTML=`<div class=row><label>Page <input id=nav${i} maxlength=10 value="${esc(p.label)}"></label><label>Title <input id=title${i} maxlength=24 value="${esc(p.title)}"></label><button data-page="${i},-1">↑</button><button data-page="${i},1">↓</button><button data-remove-page=${i}>Remove</button></div><div class=slotgrid>${Array.from({length:6},(_,j)=>`<label>Slot ${j+1}<select class="slot${i}" data-page=${i} data-slot=${j}><option value="0">Empty</option>${devices.map(d=>`<option value=${d.id} ${p.devices.includes(d.id)&&p.devices[j]===d.id?'selected':''}>${esc(d.label)} (${d.kind})</option>`).join('')}</select></label>`).join('')}</div>`;
 $('pages').append(div)});
 $('pageCount').textContent=cfg.pages.length+' / 8 pages, plus Settings. Six device slots per page.';
 $('labels').innerHTML=devices.map(d=>`<label><input id=label${d.id} maxlength=32 placeholder="${esc(d.label)}" value="${esc(cfg.labels.find(x=>x.id===d.id)?.label||'')}"><button type=button data-reset-label=${d.id}>Reset</button></label>`).join('');
 renderSleep();renderSelection();
}
async function req(path,method='GET',body=null,type='application/json'){const h={};if(method!=='GET'){h['X-Dashboard-Request']='1';if(method!=='DELETE')h['Content-Type']=type}return fetch(path,{method,headers:h,body})}
async function getJSON(path){const r=await req(path);if(!r.ok)throw Error(await r.text());return r.json()}
async function saved(r,message){if(!r.ok){$('msg').textContent=await r.text();return}
 $('msg').textContent=message+' Restarting…';busy=true;
 for(let i=0;i<30;i++){await new Promise(resolve=>setTimeout(resolve,1500));try{const state=await getJSON('/api/display');if(!state.restarting){location.reload();return}}catch(_){}}
 $('msg').textContent='Restart is taking longer; reload this page when the panel reconnects.';busy=false;
}
$('scan').onclick=async()=>{if(busy)return;try{collect()}catch(e){return alert(e.message)}busy=true;$('scan').disabled=true;$('scanStatus').textContent='Reading Maker API devices…';
 try{catalogue=await getJSON('/api/discovery');renderSelection();$('scanStatus').textContent=catalogue.length+' devices found. Select devices, then apply the selection below.'}
 catch(e){$('scanStatus').textContent=e.message}finally{busy=false;$('scan').disabled=false}};
$('applyDevices').onclick=()=>{if(busy)return;try{collect()}catch(e){return alert(e.message)}const rows=catalogue.length?catalogue:devices.map(d=>({...d,supported:true}));
 const ids=[...document.querySelectorAll('.catalogPick:checked')].map(x=>Number(x.value));if(ids.length<1||ids.length>32)return alert('Select between 1 and 32 supported devices');
 const old=devices;devices=rows.filter(d=>ids.includes(d.id)&&d.supported).map(d=>({...d,favorite:old.find(x=>x.id===d.id)?.favorite||false}));
 cfg.pages.forEach(p=>p.devices=p.devices.filter(id=>ids.includes(id)));cfg.labels=cfg.labels.filter(x=>ids.includes(x.id));
 if(!ids.includes(cfg.sleep.motion_device_id)){cfg.sleep.enabled=false;cfg.sleep.motion_device_id=0}
 provisionDirty=true;render();$('scanStatus').textContent='Selection ready. Place devices on pages, then Save and restart to apply.'};
$('pages').onclick=e=>{const page=e.target.dataset.page,remove=e.target.dataset.removePage;if(page!==undefined||remove!==undefined){collect();
 if(remove!==undefined){if(cfg.pages.length===1)return alert('Keep at least one page');cfg.pages.splice(Number(remove),1)}
 else if(page!==undefined){const[i,delta]=page.split(',').map(Number),j=i+delta;if(j>=0&&j<cfg.pages.length)[cfg.pages[i],cfg.pages[j]]=[cfg.pages[j],cfg.pages[i]]}
 render()}};
$('pages').onchange=()=>{try{collect()}catch(e){alert(e.message);render()}};$('labels').onclick=e=>{const id=e.target.dataset.resetLabel;if(id){$('label'+id).value='';collect()}};colors.forEach(c=>$(c).oninput=()=>collect());$('font').onchange=()=>collect();
$('addPage').onclick=()=>{collect();if(cfg.pages.length>=8)return alert('Maximum eight pages');let n=1;while(cfg.pages.some(p=>p.label==='Page '+n))n++;cfg.pages.push({label:'Page '+n,title:'New page',devices:[]});render()};
$('save').onclick=async()=>{if(busy)return;collect();if(cfg.sleep.enabled&&!cfg.sleep.motion_device_id)return alert('Choose a wake sensor first');
 try{const path=provisionDirty?'/api/provision':'/api/config',data=provisionDirty?{device_ids:devices.map(d=>d.id),layout:cfg}:cfg;
 if(stagedBackground){const ir=await req('/api/background','POST',stagedBackground,'application/octet-stream');if(!ir.ok)throw Error(await ir.text())}else if(removeBackground){const ir=await req('/api/background','DELETE');if(!ir.ok)throw Error(await ir.text())}
 const r=await req(path,'POST',JSON.stringify(data));await saved(r,'Saved.')}catch(e){$('msg').textContent=e.message}};
$('restore').onclick=async()=>{if(!confirm('Reset pages, colors, and sleep settings; keep selected devices and remove the background?'))return;
 try{await saved(await req('/api/reset','POST','{}'),'Defaults restored.')}catch(e){$('msg').textContent=e.message}};
$('download').onclick=()=>{collect();const a=document.createElement('a');a.href=URL.createObjectURL(new Blob([JSON.stringify(cfg,null,2)],{type:'application/json'}));a.download='dashboard-layout.json';a.click();URL.revokeObjectURL(a.href)};
$('upload').onchange=async e=>{try{const data=JSON.parse(await e.target.files[0].text()),keys=['background','panel','text','muted','accent','active','updating','warning','info'];
 if(data.version!==1||!data.theme||keys.slice(0,5).some(k=>!Number.isInteger(data.theme[k])||data.theme[k]<0||data.theme[k]>0xffffff)||keys.slice(5).some(k=>data.theme[k]!==undefined&&(!Number.isInteger(data.theme[k])||data.theme[k]<0||data.theme[k]>0xffffff))||!Array.isArray(data.pages)||data.pages.length<1||data.pages.length>8||!Array.isArray(data.labels)||!['normal','large'].includes(data.font)||data.pages.some(p=>!p||typeof p.label!=='string'||typeof p.title!=='string'||!Array.isArray(p.devices)||p.devices.length>6)||data.labels.some(x=>!x||!Number.isInteger(x.id)||typeof x.label!=='string'))throw Error();
 data.sleep??=sleepDefaults();Object.assign(data.theme,{active:data.theme.active??0xf2b866,updating:data.theme.updating??0x25453f,warning:data.theme.warning??0xf27878,info:data.theme.info??0x6ba8ff});cfg=data;render()}catch(_){alert('Unsupported or invalid layout JSON');e.target.value=''}};
$('image').onchange=e=>{const f=e.target.files[0];imageReady=false;if(!f)return;const im=new Image();im.onload=()=>{imageReady=true;const s=Math.max(400/im.width,240/im.height),w=im.width*s,h=im.height*s;ctx.clearRect(0,0,400,240);ctx.drawImage(im,(400-w)/2,(240-h)/2,w,h)};im.onerror=()=>{$('msg').textContent='Could not read image. Try PNG or JPEG.'};im.src=URL.createObjectURL(f)};
$('uploadImage').onclick=()=>{if(!imageReady)return alert('Choose an image and wait for the preview');const px=ctx.getImageData(0,0,400,240).data,out=new Uint8Array(192000);
 for(let i=0,j=0;i<px.length;i+=4,j+=2){const v=((px[i]>>3)<<11)|((px[i+1]>>2)<<5)|(px[i+2]>>3);out[j]=v&255;out[j+1]=v>>8}stagedBackground=out;removeBackground=false;$('msg').textContent='Background ready; press Save and restart to apply.'};
$('removeImage').onclick=()=>{stagedBackground=null;removeBackground=true;$('msg').textContent='Background removal ready; press Save and restart to apply.'};
async function displayStatus(){try{const s=await getJSON('/api/display');$('sleepStatus').textContent='On device: '+(s.sleeping?'screen asleep':'screen awake')+(s.enabled?(s.wake_sensor_ready?' · wake sensor ready':' · sleep paused: waiting for a fresh, online motion reading'):' · automatic sleep disabled')}catch(e){$('sleepStatus').textContent=e.message}}
$('wakeNow').onclick=async()=>{try{const r=await req('/api/display/wake','POST','{}');if(!r.ok)throw Error(await r.text());setTimeout(displayStatus,1200)}catch(e){$('sleepStatus').textContent=e.message}};
$('refreshStatus').onclick=displayStatus;
async function load(){devices=await getJSON('/api/devices');cfg=await getJSON('/api/config');cfg.sleep??=sleepDefaults();Object.assign(cfg.theme,{active:cfg.theme.active??0xf2b866,updating:cfg.theme.updating??0x25453f,warning:cfg.theme.warning??0xf27878,info:cfg.theme.info??0x6ba8ff});render();await displayStatus();
 const r=await req('/api/background');if(r.ok){const b=await r.arrayBuffer();if(b.byteLength===192000){const px=new Uint8Array(b),im=ctx.createImageData(400,240);for(let i=0,j=0;i<px.length;i+=2,j+=4){const v=px[i]|(px[i+1]<<8);im.data[j]=(((v>>11)&31)*255/31)|0;im.data[j+1]=(((v>>5)&63)*255/63)|0;im.data[j+2]=((v&31)*255/31)|0;im.data[j+3]=255}ctx.putImageData(im,0,0)}}}
load().catch(e=>{$('msg').textContent='Could not load dashboard: '+e.message});
</script>)HTML";
}  // namespace esphome::dashboard_config
