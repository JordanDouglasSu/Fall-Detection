// =====================================================================
//  PIPER fall node — web dashboard (served from the ESP32)
//  Live thermal view, detector state, event log, and a zone painter for
//  marking floor / bed / ignore areas without reflashing.
// =====================================================================
#pragma once
#include <pgmspace.h>

const char INDEX_HTML[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>PIPER Fall Sensor</title>
<style>
:root{--bg:#111214;--card:#1b1c1f;--line:#2e3035;--text:#eceef2;--muted:#8b9099;
--ok:#22c55e;--warn:#f59e0b;--alert:#ef4444;--info:#60a5fa}
*{box-sizing:border-box}
body{margin:0;padding:16px;background:var(--bg);color:var(--text);
font:14px/1.4 -apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,sans-serif;display:flex;justify-content:center}
.wrap{width:100%;max-width:520px}
.card{background:var(--card);border:1px solid var(--line);border-radius:12px;padding:16px;margin-bottom:12px}
.head{display:flex;justify-content:space-between;align-items:baseline;margin-bottom:10px}
h1{font-size:17px;margin:0}
.sub{color:var(--muted);font-size:12px}
.badge{padding:12px;border-radius:8px;text-align:center;font-weight:700;letter-spacing:.03em;margin-bottom:12px;border:1px solid}
.b-ok{background:rgba(34,197,94,.12);color:var(--ok);border-color:var(--ok)}
.b-idle{background:rgba(139,144,153,.12);color:var(--muted);border-color:var(--line)}
.b-warn{background:rgba(245,158,11,.15);color:var(--warn);border-color:var(--warn)}
.b-alert{background:rgba(239,68,68,.22);color:var(--alert);border-color:var(--alert);animation:blink .7s infinite alternate}
@keyframes blink{from{opacity:.65}to{opacity:1}}
.view{position:relative;background:#000;border-radius:8px;overflow:hidden;border:1px solid var(--line)}
canvas{width:100%;height:auto;aspect-ratio:4/3;display:block;touch-action:none}
.row{display:flex;gap:8px;flex-wrap:wrap;align-items:center;margin-top:10px}
.grid{display:grid;grid-template-columns:repeat(4,1fr);gap:8px;margin-top:12px}
.stat{background:#0c0d0f;border:1px solid var(--line);border-radius:6px;padding:7px 4px;text-align:center}
.lbl{font-size:10px;color:var(--muted);text-transform:uppercase;letter-spacing:.04em}
.val{font-weight:700;margin-top:2px;font-variant-numeric:tabular-nums}
button{padding:9px 12px;border-radius:6px;border:1px solid #474a52;background:#26282d;color:var(--text);font-size:13px;cursor:pointer}
button:active{background:#33363c}
button.primary{border-color:var(--alert);color:#fff;background:#7f1d1d}
button.sel{outline:2px solid var(--info)}
label{font-size:12px;color:var(--muted);cursor:pointer;display:flex;gap:4px;align-items:center}
.ev{font-family:ui-monospace,Consolas,monospace;font-size:12px;color:#c9ccd3;max-height:170px;overflow:auto;margin:0;padding:0;list-style:none}
.ev li{padding:3px 0;border-bottom:1px solid #24262b}
.hint{color:var(--muted);font-size:12px;margin-top:8px}
textarea{width:100%;height:120px;background:#0c0d0f;color:#c9ccd3;border:1px solid var(--line);border-radius:6px;font:11px ui-monospace,Consolas,monospace;margin-top:8px}
.hidden{display:none}
</style>
</head>
<body>
<div class="wrap">
 <div class="card">
  <div class="head"><h1>PIPER Fall Sensor</h1><span class="sub" id="node">—</span></div>
  <div id="badge" class="badge b-idle">CONNECTING…</div>
  <div class="view"><canvas id="cv" width="320" height="240"></canvas></div>
  <div class="row">
   <label><input type="checkbox" id="smooth" checked>Smooth</label>
   <label><input type="checkbox" id="zones" checked>Zones</label>
   <label><input type="checkbox" id="mask">Body mask</label>
   <span class="sub" id="cell" style="margin-left:auto">row – col –</span>
  </div>
  <div class="grid">
   <div class="stat"><div class="lbl">Posture</div><div class="val" id="posture">–</div></div>
   <div class="stat"><div class="lbl">Motion</div><div class="val" id="motion">–</div></div>
   <div class="stat"><div class="lbl">On floor</div><div class="val" id="floor">–</div></div>
   <div class="stat"><div class="lbl">H / W</div><div class="val" id="ratio">–</div></div>
   <div class="stat"><div class="lbl">Top row</div><div class="val" id="top">–</div></div>
   <div class="stat"><div class="lbl">Room</div><div class="val" id="bg">–</div></div>
   <div class="stat"><div class="lbl">Frames/s</div><div class="val" id="fps">–</div></div>
   <div class="stat"><div class="lbl">MQTT</div><div class="val" id="mqtt">–</div></div>
  </div>
  <div class="row">
   <button class="primary" onclick="act('/clear')">Clear alert</button>
   <button onclick="act('/ok')">Person says they're OK</button>
  </div>
 </div>

 <div class="card">
  <div class="head"><h1>Events</h1><span class="sub">newest first</span></div>
  <ul class="ev" id="events"><li>—</li></ul>
 </div>

 <div class="card">
  <div class="head"><h1>Room zones</h1>
   <button id="editBtn" onclick="toggleEdit()">Edit zones</button></div>
  <div id="editor" class="hidden">
   <div class="row">
    <button class="brush" data-z="F" onclick="brush(this)">Floor</button>
    <button class="brush" data-z="B" onclick="brush(this)">Bed / sofa</button>
    <button class="brush" data-z="X" onclick="brush(this)">Ignore</button>
    <button class="brush" data-z="." onclick="brush(this)">Erase</button>
   </div>
   <div class="row">
    <button onclick="saveMask()">Save to sensor</button>
    <button onclick="revertMask()">Undo changes</button>
    <button onclick="showCode()">Copy for config.h</button>
    <button onclick="resetMask()">Reset to config.h</button>
   </div>
   <div class="hint">Paint on the image above. Floor = where the floor is visible.
    A person lying there counts as "on the floor". Bed/sofa = resting, never a fall.
    Ignore = heaters, TV, sunny windows.</div>
   <textarea id="code" class="hidden" readonly></textarea>
  </div>
  <div class="hint" id="zoneInfo"></div>
 </div>
</div>

<script>
const W=32,H=24,cv=document.getElementById('cv'),ctx=cv.getContext('2d');
const off=document.createElement('canvas');off.width=W;off.height=H;const octx=off.getContext('2d');
let mask='',savedMask='',editing=false,curBrush='F',painting=false,last=null;
const $=id=>document.getElementById(id);
const ZC={F:'rgba(96,165,250,.28)',B:'rgba(167,139,250,.35)',X:'rgba(120,120,120,.55)'};

function cmap(t,mn,mx){const n=Math.max(0,Math.min(1,(t-mn)/((mx-mn)||1)));
 return[255*Math.sqrt(n)|0,255*Math.pow(n,3)|0,255*Math.sin(n*Math.PI)|0];}

async function loadConfig(){
 try{const c=await(await fetch('/config',{cache:'no-store'})).json();
  mask=c.mask;savedMask=c.mask;$('node').textContent=c.node+' · '+c.mount+' mount · '+c.ip;
  $('zoneInfo').textContent='Zones: '+(c.mask_source==='saved'?'saved on sensor':'from config.h');
 }catch(e){setTimeout(loadConfig,2000);}
}

function draw(d){
 const img=octx.createImageData(W,H);
 for(let i=0;i<W*H;i++){const[r,g,b]=cmap(d.pixels[i],d.min,d.max);
  img.data[i*4]=r;img.data[i*4+1]=g;img.data[i*4+2]=b;img.data[i*4+3]=255;}
 octx.putImageData(img,0,0);
 ctx.imageSmoothingEnabled=$('smooth').checked;
 ctx.drawImage(off,0,0,cv.width,cv.height);
 const sx=cv.width/W,sy=cv.height/H;
 if(($('zones').checked||editing)&&mask.length===W*H){
  for(let i=0;i<W*H;i++){const z=mask[i];if(ZC[z]){ctx.fillStyle=ZC[z];ctx.fillRect((i%W)*sx,((i/W)|0)*sy,sx,sy);}}
 }
 if($('mask').checked&&d.body){
  ctx.fillStyle='rgba(255,255,255,.25)';
  for(let i=0;i<W*H;i++)if(d.body[i]==='1')ctx.fillRect((i%W)*sx,((i/W)|0)*sy,sx,sy);
 }
 if(d.present&&d.box[0]>=0){
  ctx.strokeStyle=d.alert?'#ef4444':(d.state==='DESCENT'||d.state==='ON_FLOOR')?'#f59e0b':'#22c55e';
  ctx.lineWidth=2;ctx.strokeRect(d.box[0]*sx,d.box[1]*sy,(d.box[2]-d.box[0]+1)*sx,(d.box[3]-d.box[1]+1)*sy);
  ctx.fillStyle=ctx.strokeStyle;ctx.beginPath();ctx.arc((d.f.cx+.5)*sx,(d.f.cy+.5)*sy,4,0,7);ctx.fill();
  ctx.setLineDash([4,3]);ctx.beginPath();ctx.moveTo(0,(d.f.top+.5)*sy);ctx.lineTo(cv.width,(d.f.top+.5)*sy);ctx.stroke();ctx.setLineDash([]);
 }
}

function badge(d){
 const b=$('badge');
 if(d.age>3000){b.textContent='NO SENSOR DATA';b.className='badge b-alert';return;}
 if(d.alert){b.textContent='FALL DETECTED — '+d.reason.replace(/_/g,' ');b.className='badge b-alert';return;}
 if(d.state==='DESCENT'){b.textContent='SUDDEN DROP — CHECKING';b.className='badge b-warn';return;}
 if(d.state==='ON_FLOOR'){b.textContent='PERSON ON FLOOR — CHECKING';b.className='badge b-warn';return;}
 if(!d.present){b.textContent='ROOM EMPTY';b.className='badge b-idle';return;}
 b.textContent='NORMAL';b.className='badge b-ok';
}

async function loop(){
 try{
  const d=await(await fetch('/data'+($('mask').checked?'?body=1':''),{cache:'no-store'})).json();
  draw(d);badge(d);
  $('posture').textContent=d.present?d.posture+(d.still?' · still':''):'–';
  $('motion').textContent=d.present?d.f.motion.toFixed(2):'–';
  $('floor').textContent=d.present?Math.round(d.f.floor*100)+'%':'–';
  $('ratio').textContent=d.present?d.f.ratio.toFixed(2):'–';
  $('top').textContent=d.present?d.f.top.toFixed(1):'–';
  $('bg').textContent=d.bg.toFixed(1)+'°C';
  $('fps').textContent=d.fps.toFixed(1);
  $('mqtt').textContent=d.mqtt;
 }catch(e){}
 setTimeout(loop,150);
}

async function events(){
 try{const e=await(await fetch('/events',{cache:'no-store'})).json();
  $('events').innerHTML=e.length?e.map(x=>'<li>'+fmt(x.ago)+' ago — '+x.text+'</li>').join(''):'<li>No events yet</li>';
 }catch(e){}
 setTimeout(events,2000);
}
function fmt(ms){const s=Math.round(ms/1000);if(s<60)return s+'s';const m=Math.floor(s/60);if(m<60)return m+'m '+(s%60)+'s';return Math.floor(m/60)+'h '+(m%60)+'m';}
async function act(u){try{await fetch(u,{method:'POST'});}catch(e){}}

// ---- zone editor ----
function toggleEdit(){editing=!editing;$('editor').classList.toggle('hidden',!editing);
 $('editBtn').textContent=editing?'Done':'Edit zones';if(editing)brush(document.querySelector('.brush'));}
function brush(el){curBrush=el.dataset.z;document.querySelectorAll('.brush').forEach(b=>b.classList.toggle('sel',b===el));}
function cellAt(ev){const r=cv.getBoundingClientRect();
 const x=Math.floor((ev.clientX-r.left)/r.width*W),y=Math.floor((ev.clientY-r.top)/r.height*H);
 return(x>=0&&x<W&&y>=0&&y<H)?{x,y}:null;}
function paint(c){if(!c||mask.length!==W*H)return;const i=c.y*W+c.x;
 mask=mask.substring(0,i)+curBrush+mask.substring(i+1);}
cv.addEventListener('pointerdown',e=>{if(!editing)return;painting=true;cv.setPointerCapture(e.pointerId);paint(cellAt(e));});
cv.addEventListener('pointermove',e=>{const c=cellAt(e);if(c)$('cell').textContent='row '+c.y+' col '+c.x;
 if(editing&&painting)paint(c);});
cv.addEventListener('pointerup',()=>painting=false);
async function saveMask(){try{const r=await fetch('/mask',{method:'POST',body:mask});
 if(r.ok){savedMask=mask;$('zoneInfo').textContent='Zones: saved on sensor';}else alert('Save failed');}catch(e){alert('Save failed');}}
function revertMask(){mask=savedMask;}
async function resetMask(){if(!confirm('Go back to the zones in config.h?'))return;
 try{await fetch('/mask',{method:'POST',body:'default'});}catch(e){}loadConfig();}
function showCode(){let s='static const char* const ROOM_MASK[24] = {\n';
 for(let y=0;y<H;y++)s+='  "'+mask.substr(y*W,W)+'",  // '+y+'\n';s+='};';
 const t=$('code');t.value=s;t.classList.remove('hidden');t.select();
 try{navigator.clipboard.writeText(s);}catch(e){}}

loadConfig();loop();events();
</script>
</body>
</html>
)rawliteral";
