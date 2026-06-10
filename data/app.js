/* -- State ------------------------------------------------------- */
let cNid=null,chart=null,cMet='power',socket=null,wsOk=false,fbT=null;
let cc = null;
let NC=[],gwTimeSet=false,gwTime='--:--:--',gwNtpSynced=false;
let pendingCmd={};
let relayOffAt={};
let costRate=parseFloat(localStorage.getItem('pt-costRate'))||12.00;
let authToken=localStorage.getItem('pt-token')||null;
const energyAnchor={}; // nodeId -> {wh, ts}
const CONVERGE_MS=5*60*1000;
function _eaKey(id){return'pt-ea-'+id;}
function recordEnergyAnchor(id,wh){
  if(energyAnchor[id])return;
  try{const s=localStorage.getItem(_eaKey(id));if(s){energyAnchor[id]=JSON.parse(s);return;}}catch(e){}
  const a={wh,ts:Date.now()};
  energyAnchor[id]=a;
  try{localStorage.setItem(_eaKey(id),JSON.stringify(a));}catch(e){}
}
function clearEnergyAnchor(id){
  delete energyAnchor[id];
  try{localStorage.removeItem(_eaKey(id));}catch(e){}
}
const MIN_DELTA_WH=5; // require 5 Wh before trusting rate -- prevents 1 Wh integer ticks over short windows from blowing up the estimate
// Returns {cph, state:'waiting'|'converging'|'ready'}
function getEnergyRateInfo(id,currentWh,online){
  if(!online)return{cph:null,state:'waiting'};
  const a=energyAnchor[id];
  if(!a)return{cph:null,state:'waiting'};
  if(currentWh<a.wh){clearEnergyAnchor(id);return{cph:null,state:'waiting'};}
  const elapsedMs=Date.now()-a.ts;
  if(elapsedMs<3000)return{cph:null,state:'waiting'};
  const deltaWh=currentWh-a.wh;
  if(deltaWh<MIN_DELTA_WH)return{cph:null,state:'converging'};
  const elapsedH=elapsedMs/3600000;
  const cph=(deltaWh/elapsedH/1000)*costRate;
  const state=elapsedMs<CONVERGE_MS?'converging':'ready';
  return{cph,state};
}

/* -- Auth: save raw fetch ref and patch window.fetch for token injection -- */
window._authFetch=window.fetch.bind(window);
(function(){
  const _f=window._authFetch;
  window.fetch=async function(url,opts){
    opts=opts||{};
    if(authToken){opts={...opts,headers:{...(opts.headers||{}),'X-Auth-Token':authToken}};}
    const r=await _f(url,opts);
    if(r.status===401&&typeof url==='string'&&!url.includes('/api/login')){
      authToken=null;localStorage.removeItem('pt-token');showLoginOverlay();
    }
    return r;
  };
})();

const MC={
  power:  {l:'Power (W)',  c:'#118ab2', k:'p', n:'power'},
  voltage:{l:'Voltage (V)',c:'#ffd166', k:'v', n:'voltage'},
  current:{l:'Current (A)',c:'#06d6a0', k:'i', n:'current'}
};
const SPARK_MAX=120;
let sparkData={power:[],voltage:[],current:[]};
function clearSparkData(){sparkData={power:[],voltage:[],current:[]};}
function pushSparkPoint(n){
  const pairs=[['power','p'],['voltage','v'],['current','i']];
  pairs.forEach(([long,short])=>{
    const v=n[long]!==undefined?n[long]:n[short];
    if(v===undefined)return;
    sparkData[long].push(v);
    if(sparkData[long].length>SPARK_MAX)sparkData[long].shift();
  });
}
function loadSparkFromHistory(d){
  sparkData.power=d.map(x=>x.p).filter(v=>v!==undefined);
  sparkData.voltage=d.map(x=>x.v).filter(v=>v!==undefined);
  sparkData.current=d.map(x=>x.i).filter(v=>v!==undefined);
}
function makeSvgSparkline(vals,color){
  if(!vals||vals.length<2)return'';
  // downsample to 60 pts for path length
  const src=(vals.length>60)?vals.filter((_,i)=>i%Math.ceil(vals.length/60)===0):vals;
  const dMax=Math.max(...src);
  const range=dMax||1;
  const dispMin=0;
  const W=200,H=38,PAD_T=6;
  const pts=src.map((v,i)=>({
    x:(i/(src.length-1))*W,
    y:PAD_T+(H-PAD_T)*(1-(v-dispMin)/range)
  }));
  // catmull-rom smooth path
  let line='M '+pts[0].x.toFixed(1)+','+pts[0].y.toFixed(1);
  for(let i=0;i<pts.length-1;i++){
    const p0=pts[Math.max(0,i-1)],p1=pts[i],p2=pts[i+1],p3=pts[Math.min(pts.length-1,i+2)];
    const cp1x=(p1.x+(p2.x-p0.x)/6).toFixed(1),cp1y=(p1.y+(p2.y-p0.y)/6).toFixed(1);
    const cp2x=(p2.x-(p3.x-p1.x)/6).toFixed(1),cp2y=(p2.y-(p3.y-p1.y)/6).toFixed(1);
    line+=` C ${cp1x},${cp1y} ${cp2x},${cp2y} ${p2.x.toFixed(1)},${p2.y.toFixed(1)}`;
  }
  const fill=line+` L ${W},${H} L 0,${H} Z`;
  const gid='spg'+color.replace(/[^a-z0-9]/gi,'');
  return`<svg viewBox="0 0 ${W} ${H}" preserveAspectRatio="none" width="100%" height="100%" aria-hidden="true">`
    +`<defs><linearGradient id="${gid}" x1="0" y1="0" x2="0" y2="1">`
    +`<stop offset="0%" stop-color="${color}" stop-opacity="0.25"/>`
    +`<stop offset="100%" stop-color="${color}" stop-opacity="0"/>`
    +`</linearGradient></defs>`
    +`<path d="${fill}" fill="url(#${gid})"/>`
    +`<path d="${line}" fill="none" stroke="${color}" stroke-width="1.8" stroke-linejoin="round" stroke-linecap="round"/>`
    +`</svg>`;
}
const SPARK_PREVIEW=40;
function updateSparklines(){
  const map=[['sparkV','voltage','#ffd166'],['sparkI','current','#06d6a0'],['sparkP','power','#118ab2']];
  map.forEach(([id,key,color])=>{
    const el=$(id);if(el)el.innerHTML=makeSvgSparkline(sparkData[key].slice(-SPARK_PREVIEW),color);
  });
}
function expandMetric(key){
  swChart(key);
  const panel=$('det-chart-panel');
  if(panel)panel.scrollIntoView({behavior:'smooth',block:'nearest'});
}
const LOG_MAX_LINES=200;
const LOG_PREFIX_CLASS={
  'GW-UL':'ok','GW-BCN':'ok','GW-DL':'ok','GW-CW':'ok','GW-TDMA':'ok',
  'NODE-TDMA':'ok','NODE-TX':'ok','NODE-DL':'ok','NODE-JOIN':'ok',
  'NODE-RTC':'ok','NODE-SCHED':'ok','NODE':'ok','PZEM':'ok',
  'WIFI':'info','WEB':'info','WS':'info','FRAM':'info','FS':'info',
  'LORA':'info','CRYPTO':'info','RFID':'info','GW':'info',
};
const logSuppressed=new Set();
const SL=['','Waiting','Active'];
const SC=['','sched-badge waiting','sched-badge active'];
const NUDGE_ICO='<svg width="10" height="10" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2"><circle cx="12" cy="12" r="10"/><line x1="12" y1="8" x2="12" y2="12"/><line x1="12" y1="16" x2="12.01" y2="16"/></svg>';
const NUDGED_ICO='<svg width="10" height="10" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round"><polyline points="20 6 9 17 4 12"/></svg>';
const RELAY_WARN_W=5;
const SUPERFRAME_MS=3000;

/* -- Classifier -------------------------------------------------- */
const CLF_NAMES=['Resistive','Capacitive','Motor','Fan','SMPS','Lighting','No Load'];
const CLF_COLORS=['#8892a4','#4fc3f7','#ff9f43','#06d6a0','#ff6b6b','#ffd166','#4a5568'];

// Returns [r,g,b] for a KNN distSq value. Maps 0..0.30 to green..red in four bands.
function knnStrengthRgb(dSq){
  const s=Math.max(0,Math.min(1,1-dSq/0.30));
  return s>0.75?[0,229,160]:s>0.50?[255,209,102]:s>0.25?[255,159,67]:[255,56,96];
}

function clfBadgeHtml(clf,full){
  if(!clf||!clf.supported)return'';
  if(clf.pending)return`<div class="nc-clf"><span class="clf-pending M">Analyzing...</span></div>`;
  const name=CLF_NAMES[clf.classId]||'Unknown';
  const color=CLF_COLORS[clf.classId]||'#8892a4';
  if(!full||clf.classId===6)return`<div class="nc-clf"><span class="clf-badge M" style="--bc:${color}">${name}</span></div>`;
  const conf=Math.min(7,Math.max(0,clf.confidence||0));
  const pct=Math.round(conf/7*100);
  const transHtml=clf.transient?'<span class="clf-trans M">~</span>':'';
  return`<div class="nc-clf"><span class="clf-badge M" style="--bc:${color}">${name}</span>${transHtml}<div class="clf-bar-wrap"><div class="clf-bar-fill" style="width:${pct}%;background:${color}"></div></div><span class="clf-bar-pct M" style="color:${color}">${pct}%</span></div>`;
}

/* -- Theme ------------------------------------------------------- */
const SUN_PATHS='<circle cx="12" cy="12" r="5"/><line x1="12" y1="1" x2="12" y2="3"/><line x1="12" y1="21" x2="12" y2="23"/><line x1="4.22" y1="4.22" x2="5.64" y2="5.64"/><line x1="18.36" y1="18.36" x2="19.78" y2="19.78"/><line x1="1" y1="12" x2="3" y2="12"/><line x1="21" y1="12" x2="23" y2="12"/><line x1="4.22" y1="19.78" x2="5.64" y2="18.36"/><line x1="18.36" y1="5.64" x2="19.78" y2="4.22"/>';
const MOON_PATH='<path d="M21 12.79A9 9 0 1 1 11.21 3 7 7 0 0 0 21 12.79z"/>';

function getTheme(){return localStorage.getItem('pt-theme')||(window.matchMedia('(prefers-color-scheme:light)').matches?'light':'dark');}
function applyTheme(t){
  document.documentElement.setAttribute('data-theme',t);
  $('themeIco').innerHTML=t==='dark'?MOON_PATH:SUN_PATHS;
  localStorage.setItem('pt-theme',t);
  refreshChartTheme();
}
function toggleTheme(){applyTheme(getTheme()==='dark'?'light':'dark');}

function refreshChartTheme(){
  if(!chart)return;
  const grid=getCS('--chart-grid'),tx=getCS('--chart-tx'),
        tipBg=getCS('--chart-tip-bg'),tipBd=getCS('--chart-tip-bd');
  chart.options.plugins.tooltip.backgroundColor=tipBg;
  chart.options.plugins.tooltip.borderColor=tipBd;
  chart.options.plugins.tooltip.titleColor=tx;
  chart.options.plugins.tooltip.bodyColor=tx;
  chart.options.scales.x.grid.color=grid;
  chart.options.scales.x.ticks.color=tx;
  chart.options.scales.y.grid.color=grid;
  chart.options.scales.y.ticks.color=tx;
  chart.update('none');
}

/* -- Helpers ----------------------------------------------------- */
function $(id){return document.getElementById(id);}
function sT(id,v){const e=$(id);if(e)e.textContent=v;}
function sB(id,v,mx){const e=$(id);if(e)e.style.width=Math.min(100,Math.max(0,(v/mx)*100))+'%';}
function fU(s){if(!s||s<0)return'--';return Math.floor(s/3600)+'h '+Math.floor((s%3600)/60)+'m '+(s%60)+'s';}
function fP(w){return w>=1000?(w/1000).toFixed(1)+'k':w.toFixed(0);}
function esc(s){const d=document.createElement('div');d.textContent=s;return d.innerHTML.replace(/"/g,'&quot;');}
function pad2(n){return String(n).padStart(2,'0');}
function rssiToBars(r){if(r>=-55)return 4;if(r>=-65)return 3;if(r>=-75)return 2;return 1;}

/* -- Clock ------------------------------------------------------- */
function updateClock(){
  const d=new Date();
  const t=pad2(d.getHours())+':'+pad2(d.getMinutes());
  const sc=$('sysClock');if(sc)sc.textContent=t;
}
setInterval(updateClock,10000);updateClock();

/* -- Toast ------------------------------------------------------- */
function showToast(msg,type,duration){
  type=type||'info';duration=duration||4000;
  const tc=$('toastContainer');if(!tc)return;
  const t=document.createElement('div');
  t.className='toast toast-'+type;
  t.setAttribute('role','status');
  t.textContent=msg;
  tc.appendChild(t);
  requestAnimationFrame(()=>requestAnimationFrame(()=>t.classList.add('show')));
  setTimeout(()=>{
    t.classList.remove('show');
    t.addEventListener('transitionend',()=>t.remove(),{once:true});
  },duration);
}

/* -- Confirm dialog ---------------------------------------------- */
function openConfirm(title,msg,onYes,yesLabel,dangerous,anchor){
  yesLabel=yesLabel||'Confirm';dangerous=dangerous||false;
  const dlg=$('confirmDialog');if(!dlg)return;
  sT('dlgTitle',title);
  sT('dlgMsg',msg);
  const yesBtn=$('dlgYes'),noBtn=$('dlgNo');
  yesBtn.textContent=yesLabel;
  yesBtn.className='btn '+(dangerous?'btn-danger':'btn-primary')+' M';
  yesBtn.style.cssText='margin-top:0;width:auto;padding:8px 20px';
  const close=()=>{
    dlg.classList.remove('dlg-anchored');
    dlg.style.cssText='';
    dlg.close();
  };
  yesBtn.onclick=()=>{close();onYes();};
  noBtn.onclick=close;
  const onBdClick=e=>{if(e.target===dlg)close();};
  dlg.addEventListener('click',onBdClick,{once:true});
  dlg.showModal();
  if(anchor){
    dlg.classList.add('dlg-anchored');
    const r=anchor.getBoundingClientRect();
    const dw=dlg.offsetWidth||280;
    const dh=dlg.offsetHeight||160;
    let left=r.left;
    let top=r.bottom+8;
    if(left+dw>window.innerWidth-8)left=window.innerWidth-dw-8;
    if(left<8)left=8;
    if(top+dh>window.innerHeight-8)top=r.top-dh-8;
    if(top<8)top=r.bottom+8;
    dlg.style.cssText='margin:0;top:'+top+'px;left:'+left+'px';
  }
}

/* -- Stubs for simulation.js compatibility ----------------------- */
function showConfirm(id){}
function hideConfirm(id){}
function toggleWifiDrawer(){}
function toggleGwCtrlDrawer(){}

/* -- Prompt wrappers --------------------------------------------- */
function promptClearEnergy(a){
  openConfirm('Clear Energy','Reset energy total for '+cc.label+'? This cannot be undone.',
    ()=>wsSend({cmd:'clear_energy',node:cNid}),'Clear',true,a);
}
function promptClearAllEnergy(a){
  openConfirm('Clear All Energy','Reset energy totals for all nodes? This cannot be undone.',
    ()=>wsSend({cmd:'clear_all_energy'}),'Clear All',true,a);
}
function promptForget(a){
  openConfirm('Forget WiFi','Clear saved credentials and restore AP mode?',
    wifiDoForget,'Forget',true,a);
}
function promptKeyRegen(a){
  openConfirm('Regenerate Key','This will invalidate all provisioned node keys. Nodes must be reprovisioned.',
    execKeyRegen,'Regenerate',true,a);
}
function promptReboot(a){
  openConfirm('Reboot Gateway','Reboot the gateway firmware now?',
    execReboot,'Reboot',true,a);
}
function promptLogout(a){
  openConfirm('Log Out','Log out of the dashboard?',
    doLogout,'Log Out',false,a);
}

/* -- Favorites --------------------------------------------------- */
function getFavs(){try{return JSON.parse(localStorage.getItem('pt-favs')||'[]');}catch(e){return[];}}
function saveFavs(f){localStorage.setItem('pt-favs',JSON.stringify(f));}
function isFav(id){return getFavs().includes(id);}
function toggleFav(id,ev){ev.stopPropagation();let f=getFavs();if(f.includes(id))f=f.filter(x=>x!==id);else f.push(id);saveFavs(f);renderGrid(NC);}

/* -- Serial log -------------------------------------------------- */
function buildLogFilterRow(){
  const row=$('logFilterRow');
  if(!row)return;
  Object.keys(LOG_PREFIX_CLASS).forEach(p=>{
    const chip=document.createElement('span');
    chip.className='log-filter-chip';
    chip.textContent=p;
    chip.dataset.prefix=p;
    chip.addEventListener('click',()=>toggleLogFilter(p));
    row.appendChild(chip);
  });
}
function toggleLogFilter(prefix){
  if(logSuppressed.has(prefix))logSuppressed.delete(prefix);
  else logSuppressed.add(prefix);
  const chip=document.querySelector(`.log-filter-chip[data-prefix="${prefix}"]`);
  if(chip)chip.classList.toggle('suppressed',logSuppressed.has(prefix));
}
function appendLog(line){
  const box=$('serialLogBox');
  if(!box)return;
  const m=line.match(/^\[([A-Z0-9-]+)\]/);
  if(m&&logSuppressed.has(m[1]))return;
  const cls=/err|fail|error/i.test(line)?'err':(m?(LOG_PREFIX_CLASS[m[1]]||'ts'):'ts');
  const div=document.createElement('div');
  div.className='log-line';
  const span=document.createElement('span');
  span.className=cls;
  span.textContent=line;
  div.appendChild(span);
  box.appendChild(div);
  while(box.childElementCount>LOG_MAX_LINES)box.removeChild(box.firstChild);
  const as=$('logAutoScroll');
  if(as&&as.checked)box.scrollTop=box.scrollHeight;
}

/* -- WebSocket --------------------------------------------------- */
function connectWS(){
  const u=(location.protocol==='https:'?'wss:':'ws:')+'//'+location.host+'/ws';
  socket=new WebSocket(u);
  socket.onopen=()=>{
    wsOk=true;wsDot(true);
    clearInterval(fbT);fbT=null;
    socket.send(JSON.stringify({cmd:'auth',token:authToken||''}));
  };
  socket.onmessage=e=>{try{onMsg(JSON.parse(e.data));}catch(x){}};
  socket.onclose=()=>{wsOk=false;wsDot(false);startFallback();setTimeout(connectWS,3000);};
  socket.onerror=()=>socket.close();
}
function wsSend(o){if(wsOk&&socket&&socket.readyState===1)socket.send(JSON.stringify(o));}
function wsDot(ok){
  const el=$('wsStatus');if(!el)return;
  el.className='hdr-badge M '+(ok?'ws-ok':'ws-err');
  el.innerHTML=`<svg width="8" height="8" viewBox="0 0 10 10" style="display:inline;margin-right:4px"><circle cx="5" cy="5" r="4" fill="currentColor"/></svg>${ok?'WS':'WS✘'}`;
}
function syncTime(){const d=new Date();wsSend({cmd:'set_time',hour:d.getHours(),minute:d.getMinutes(),second:d.getSeconds(),utcOffset:-d.getTimezoneOffset()*60});}
function doSyncTime(){
  syncTime();
  const btn=$('syncBtn');if(!btn)return;
  btn.classList.add('ac-active');
  const prev=btn.innerHTML;
  btn.innerHTML='<svg width="12" height="12" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round"><polyline points="20 6 9 17 4 12"/></svg> Synced';
  clearTimeout(btn._t);btn._t=setTimeout(()=>{btn.innerHTML=prev;btn.classList.remove('ac-active');},2500);
}

function updGwTime(){
  const el=$('gwTimeVal');if(el)el.textContent=gwTimeSet?gwTime:'Not synced';
  const w=$('gwTimeWarn');if(w)w.style.display=gwTimeSet?'none':'block';
  const src=$('gwTimeSrc');
  if(src){src.textContent='NTP';src.style.display=gwNtpSynced?'':'none';}
  const btn=$('syncBtn');if(btn)btn.style.display=gwNtpSynced?'none':'';
}

/* -- Message handler --------------------------------------------- */
function onMsg(m){
  if(m.type==='auth'){
    if(m.ok){if(!gwNtpSynced)syncTime();wsSend({cmd:'get_nodes'});}
    else{doLogout();}
    return;
  }

  if(m.timeSet!==undefined)gwTimeSet=m.timeSet;
  if(m.time)gwTime=m.time;
  if(m.ntpSynced!==undefined)gwNtpSynced=m.ntpSynced;
  updGwTime();

  if(m.type==='nodes'){
    NC=m.nodes||[];
    NC.forEach(n=>{
      if((n.relayState||0)===0){if(relayOffAt[n.id]===undefined)relayOffAt[n.id]=Date.now();}
      else delete relayOffAt[n.id];
      recordEnergyAnchor(n.id,n.energy||0);
    });
    const pg=$('pg-overview');
    if(pg&&pg.classList.contains('active'))renderGrid(NC);
    sT('idxCount',m.count||0);
    const nc=$('nodeCount');if(nc)nc.textContent=(m.count||0)+' node'+((m.count||0)!==1?'s':'');
    if(gwFeatures['transport-test'])ttPopulateNodeSel();
    if(gwHasKnn)knnPopulateNodeSel();
  }
  else if(m.type==='telemetry'){
    const n=m.node;
    const prev=NC.find(x=>x.id===n.id);
    const prevRs=prev?prev.relayState:undefined;
    if(n.relayState===1){delete relayOffAt[n.id];}
    else if(n.relayState===0&&prevRs!==0){relayOffAt[n.id]=Date.now();}
    const o={id:n.id,label:n.label,online:n.online,rssi:n.rssi,voltage:n.voltage,current:n.current,power:n.power,energy:n.energy,frequency:n.frequency,powerFactor:n.powerFactor,relayState:n.relayState,relayMode:n.relayMode,schedState:n.schedState,alarmState:n.alarmState,age:n.age,pending:n.pending,hasSched:n.hasSched,schedStart:n.schedStart,schedEnd:n.schedEnd,classifier:n.classifier};
    recordEnergyAnchor(n.id,n.energy||0);
    const i=NC.findIndex(x=>x.id===n.id);if(i>=0)Object.assign(NC[i],o);else NC.push(o);
    const pg=$('pg-overview');
    if(pg&&pg.classList.contains('active'))renderGrid(NC);
    if(cNid===n.id){updateDetail(o);addChartPoint(n);}
  }
  else if(m.type==='name_changed'){
    const i=NC.findIndex(x=>x.id===m.node);
    if(i>=0)NC[i].label=m.name;
    if(cNid===m.node&&!document.querySelector('.rename-input'))sT('detLabel',m.name);
    const pg=$('pg-overview');
    if(pg&&pg.classList.contains('active'))renderGrid(NC);
  }
  else if(m.type==='time_set'){gwTimeSet=true;if(m.time)gwTime=m.time;if(m.source)gwNtpSynced=(m.source==='ntp');updGwTime();}
  else if(m.type==='threshold_ack'){if(!m.success){if(m.node!==undefined)delete pendingCmd[m.node];showToast('Threshold command failed','error');}}
  else if(m.type==='nudge_ack'){if(!m.success){if(m.node!==undefined)delete pendingCmd[m.node];showToast('Nudge failed','error');}}
  else if(m.type==='relay_ack'||m.type==='schedule_ack'||m.type==='clear_ack'){if(!m.success){if(m.node!==undefined)delete pendingCmd[m.node];showToast('Command failed','error');}}
  else if(m.type==='energy_cleared'){const i=NC.findIndex(x=>x.id===m.node);if(i>=0)NC[i].energy=0;clearEnergyAnchor(m.node);if(cNid===m.node)updateDetail(NC.find(x=>x.id===cNid)||{});}
  else if(m.type==='all_energy_cleared'){NC.forEach(n=>n.energy=0);Object.keys(energyAnchor).forEach(clearEnergyAnchor);if(cNid){const c=NC.find(x=>x.id===cNid);if(c)updateDetail(c);}const pg=$('pg-overview');if(pg&&pg.classList.contains('active'))renderGrid(NC);}
  else if(m.type==='transport_test_ack'){onTransportTestAck(m);}
  else if(m.type==='bulk_complete'||m.type==='bulk_failed'){onBulkComplete(m);}
  else if(m.type==='log')appendLog(m.line);
}

/* -- Fallback polling -------------------------------------------- */
function startFallback(){
  if(fbT)return;
  fbT=setInterval(async()=>{
    try{
      const r=await fetch('/api/nodes');const d=await r.json();NC=d.nodes||[];
      const pg=$('pg-overview');
      if(pg&&pg.classList.contains('active'))renderGrid(NC);
      if(cNid){const n=NC.find(x=>x.id===cNid);if(n){updateDetail(n);}}
    }catch(e){}
  },3000);
}

/* -- Routing ----------------------------------------------------- */
function showPage(name){
  document.querySelectorAll('.page').forEach(p=>p.classList.toggle('active',p.id===name));
}

function updateNavActive(r){
  document.querySelectorAll('[data-route]').forEach(el=>{
    const active=el.dataset.route===r;
    el.classList.toggle('active',active);
    if(el.hasAttribute('aria-selected'))el.setAttribute('aria-selected',String(active));
  });
}

function route(){
  const h=location.hash||'#/';
  const mNode=h.match(/^#\/node\/(\d+)$/);
  const mSet=/^#\/settings/.test(h);
  const mLog=/^#\/log/.test(h);
  if(mNode)showDetail(parseInt(mNode[1]));
  else if(mSet)showSettings();
  else if(mLog)showLog();
  else showOverview();
}
window.addEventListener('hashchange',route);

function goHome(){location.hash='#/';}
function goNode(id){location.hash='#/node/'+id;}
function goSettings(){location.hash='#/settings';}
function goLog(){location.hash='#/log';}

function showOverview(){
  cNid=null;
  clearInterval(knnCountdownIv);knnCountdownIv=null;
  const cd=$('knnTrainCountdown');if(cd)cd.style.display='none';
  window.scrollTo(0,0);
  showPage('pg-overview');
  updateNavActive('overview');
  const bb=$('backBtn');if(bb)bb.style.display='none';
  const th=$('topbarHead');if(th)th.classList.remove('in-detail');
  renderGrid(NC);
  wsSend({cmd:'get_nodes'});
}

function showSettings(){
  showPage('pg-settings');
  updateNavActive('settings');
  const bb=$('backBtn');if(bb)bb.style.display='none';
  const th=$('topbarHead');if(th)th.classList.remove('in-detail');
  fetchSys();
}

function showLog(){
  showPage('pg-log');
  updateNavActive('log');
  const bb=$('backBtn');if(bb)bb.style.display='none';
  const th=$('topbarHead');if(th)th.classList.remove('in-detail');
}

/* Keep original name for simulation.js compatibility */
function showIndex(){showOverview();}

/* -- Settings tabs ----------------------------------------------- */
function setSettingsTab(tab){
  document.querySelectorAll('.stab-btn[data-tab]').forEach(b=>{
    const active=b.dataset.tab===tab;
    b.classList.toggle('active',active);
    b.setAttribute('aria-selected',String(active));
  });
  document.querySelectorAll('.stab-pane[data-tab]').forEach(p=>{
    const active=p.dataset.tab===tab;
    p.classList.toggle('active',active);
    p.style.display=active?'':'none';
  });
  localStorage.setItem('pt-stab',tab);
}

/* -- KPI strip --------------------------------------------------- */
function updateKpiStrip(){
  const online=NC.filter(n=>n.online);
  const totalPow=online.reduce((s,n)=>s+(n.power||0),0);
  const totalEnergy=NC.reduce((s,n)=>s+(n.energy||0),0);
  const totalCost=(totalEnergy/1000)*costRate;
  sT('kpiTotalPow',totalPow>=1000?(totalPow/1000).toFixed(2)+' kW':totalPow.toFixed(0)+' W');
  sT('kpiCostToday',totalCost.toFixed(2));
  const energyDisp=totalEnergy>=1000?(totalEnergy/1000).toFixed(2)+' kWh':(totalEnergy.toFixed(0)+' Wh');
  sT('kpiCostHint',costRate.toFixed(2)+' / kWh × '+energyDisp);
}

/* -- Render node grid -------------------------------------------- */
function renderGrid(ns){
  updateKpiStrip();
  const g=$('nodeGrid'),em=$('emptyState');
  if(!ns.length){g.innerHTML='';em.style.display='block';return;}
  em.style.display='none';
  const favs=getFavs();
  const sorted=[...ns].sort((a,b)=>{const af=favs.includes(a.id)?0:1,bf=favs.includes(b.id)?0:1;return af-bf||a.id-b.id;});
  const ids=new Set(sorted.map(n=>String(n.id)));
  g.querySelectorAll('.node-card').forEach(c=>{if(!ids.has(c.dataset.id))c.remove();});
  const curOrder=[...g.querySelectorAll('.node-card')].map(c=>c.dataset.id);
  const newOrder=sorted.map(n=>String(n.id));
  const orderChanged=curOrder.length!==newOrder.length||curOrder.some((v,i)=>v!==newOrder[i]);
  sorted.forEach(n=>{
    let c=g.querySelector(`.node-card[data-id="${n.id}"]`);
    const isNew=!c;
    if(isNew){c=document.createElement('div');c.className='node-card';c.dataset.id=n.id;c.setAttribute('role','listitem');c.onclick=()=>goNode(n.id);}
    const rs=n.relayState!==undefined?n.relayState:n.relay;
    const al=n.alarmState||n.alarm||0;
    const rw=n.online&&rs===0&&(n.power||0)>RELAY_WARN_W&&(Date.now()-(relayOffAt[n.id]||Date.now()))>SUPERFRAME_MS;
    const faved=isFav(n.id);
    const clf=n.classifier;
    const knn=n.knn;
    const clfKey=(!gwHasClassifier||!clf||!clf.supported)?'u':clf.pending?'p':`${clf.classId}c${clf.confidence}t${clf.transient?1:0}`;
    const knnBand=gwHasKnn&&knn?(knn.distSq||0)<0.075?0:(knn.distSq||0)<0.15?1:(knn.distSq||0)<0.225?2:3:0;
    const knnKey=gwHasKnn&&knn?`${knn.state}:${knn.label||''}:${knnBand}`:'-';
    const isTraining=gwHasKnn&&knnTrainingNodes.has(n.id);
    const fp=`${n.label}|${n.online}|${(n.voltage||0).toFixed(1)}|${(n.current||0).toFixed(2)}|${fP(n.power||0)}|${rs}|${al}|${rw}|${faved}|${n.rssi}|${clfKey}|${knnKey}|tr:${isTraining?1:0}`;
    if(c._fp!==fp){
      c._fp=fp;
      c.classList.toggle('alarm',!!al);
      c.classList.toggle('relay-warn',rw&&!al);
      c.classList.toggle('offline',!n.online);
      const bars=rssiToBars(n.rssi||0);
      let statusHtml;
      if(!n.online){
        statusHtml='<span class="nc-status off M">OFFLINE</span>';
      }else if(isTraining){
        statusHtml=`<span class="nc-status M" style="color:#f87171;background:#f871711f;border:1px solid #f871714d;display:inline-flex;align-items:center;gap:4px"><svg class="rec-dot" width="8" height="8" viewBox="0 0 24 24" fill="#f87171" stroke="none" aria-hidden="true"><circle cx="12" cy="12" r="9"/></svg>Recording</span>`;
      }else if(gwHasKnn&&knn&&knn.state==='IDENTIFIED'&&knn.label){
        const[r,g,b]=knnStrengthRgb(knn.distSq||0);
        const sc=`rgb(${r},${g},${b})`;
        const knnIco=`<svg width="13" height="13" viewBox="-2 -2 28 28" fill="${sc}" stroke="none" aria-hidden="true"><path d="M9 4C11 11 11 11 18 13C11 15 11 15 9 22C7 15 7 15 0 13C7 11 7 11 9 4Z"/><path d="M20 1C20.8 3.8 20.8 3.8 24 5C20.8 6.2 20.8 6.2 20 10C19.2 6.2 19.2 6.2 15 5C19.2 3.8 19.2 3.8 20 1Z"/></svg>`;
        statusHtml=`<span class="nc-status M" style="color:#818cf8;background:#818cf81f;border:1px solid #818cf84d;display:inline-flex;align-items:center;gap:4px">${knnIco}${esc(knn.label)}</span>`;
      }else if(gwHasClassifier&&clf&&clf.supported&&!clf.pending&&clf.classId!==undefined){
        const cName=CLF_NAMES[clf.classId]||'Unknown';
        const cColor=CLF_COLORS[clf.classId]||'#8892a4';
        statusHtml=`<span class="nc-status M" style="color:${cColor};background:${cColor}1f;border:1px solid ${cColor}4d">${cName}</span>`;
      }else{
        statusHtml='<span class="nc-status on M">ONLINE</span>';
      }
      c.innerHTML=`
        <div class="nc-head">
          <div>
            <div class="nc-id M">Node #${n.id}</div>
            <div class="nc-label">${esc(n.label)}</div>
          </div>
          ${statusHtml}
          <button class="fav-star ${faved?'faved':''}" onclick="toggleFav(${n.id},event)" aria-label="${faved?'Remove from':'Add to'} favorites">${faved?'★':'☆'}</button>
        </div>
        <div class="nc-metrics">
          <div class="nc-metric">
            <div class="nc-metric-val M" style="color:var(--volt)">${(n.voltage||0).toFixed(1)}<small style="font-size:10px;color:var(--txd)"> V</small></div>
            <div class="nc-metric-lbl">Voltage</div>
          </div>
          <div class="nc-metric">
            <div class="nc-metric-val M" style="color:${al?'var(--dg)':'var(--watt)'}">${fP(n.power||0)}<small style="font-size:10px;color:var(--txd)"> W</small></div>
            ${!al ? '<div class="nc-metric-lbl">Power</div>' : '<div class="nc-metric-lbl alarm">⚠ ALARM</div>'}
          </div>
          <div class="nc-metric">
            <div class="nc-metric-val M" style="color:var(--amp)">${(n.current||0).toFixed(2)}<small style="font-size:10px;color:var(--txd)"> A</small></div>
            <div class="nc-metric-lbl">Current</div>
          </div>
        </div>
        <div class="nc-foot M">
          <span class="nc-rssi">
            <div class="sig-bars" aria-label="${n.online?(n.rssi||'--')+' dBm':'offline'}" role="img">
              <div class="bar ${bars>=1?'lit':''}"></div>
              <div class="bar ${bars>=2?'lit':''}"></div>
              <div class="bar ${bars>=3?'lit':''}"></div>
              <div class="bar ${bars>=4?'lit':''}"></div>
            </div>
            ${n.online?(n.rssi||'--')+' dBm':'--'}
          </span>
          <button class="nudge-btn M" data-nid="${n.id}" onclick="doNudge(${n.id},event)" aria-label="Nudge node ${n.id}" ${n.online?'':'disabled style="opacity:.3;pointer-events:none"'}>
            ${NUDGE_ICO} Nudge
          </button>
          <div class="nc-relay-group" onclick="openRelayDialog(${n.id},event)" role="button" aria-label="Open relay control for ${esc(n.label)||'Node '+n.id}" style="cursor:pointer">
            <span class="nc-relay ${rs?'on':'off'}${rw?' warn':''}">Relay: ${rs?'ON':'OFF'}${rw?' ⚠':''}</span>
            <span class="nc-arrow" aria-hidden="true">
              <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round"><polyline points="9 18 15 12 9 6"/></svg>
            </span>
          </div>
        </div>
      `;
    }
    if(isNew||orderChanged)g.appendChild(c);
  });
}

/* -- Show detail ------------------------------------------------- */
function showDetail(id){
  cNid=id;
  showPage('pg-detail');
  updateNavActive('overview');
  const bb=$('backBtn');if(bb)bb.style.display='flex';
  const th=$('topbarHead');if(th)th.classList.add('in-detail');
  cMet='power';
  clearSparkData();updateSparklines();
  initChart();
  document.querySelectorAll('[data-m]').forEach(b=>{
    const active=b.dataset.m==='power';
    b.style.borderColor=active?'var(--ac)':'';
    b.style.color=active?'var(--ac)':'';
    b.style.background=active?'rgba(0,229,160,.08)':'';
  });
  fetchHistory(id);fetchSys();
  const c=NC.find(n=>n.id===id);if(c)updateDetail(c);
  cc=c;
  if(gwHasKnn)updateKnnTrainBadge();
}

/* -- Update detail view ------------------------------------------ */
function updateDetail(d){
  if(!document.querySelector('.rename-input'))sT('detLabel',d.label||('Node '+d.id));
  const ds=$('detSub');if(ds)ds.textContent=`ID: ${d.id}  •  LoRa: ${d.rssi||'--'} dBm  •  ${d.age||0}s ago`;

  const st=$('detStatus');
  if(st){st.textContent=d.online?'ONLINE':'OFFLINE';st.className='panel-badge M '+(d.online?'ac':'dg');}

  sT('dV',(d.voltage||0).toFixed(1));
  const curA=d.current||0;
  if(curA<1){sT('dI',(curA*1000).toFixed(0));sT('dIu','mA');}
  else{sT('dI',curA.toFixed(3));sT('dIu','A');}
  sT('dP',(d.power||0).toFixed(1));
  if((d.energy||0)>=10000){sT('dE',((d.energy||0)/1000).toFixed(2));sT('dEu','kWh');}
  else{sT('dE',String(d.energy||0));sT('dEu','Wh');}
  sT('dF',(d.frequency||0).toFixed(1));
  sT('dPF',(d.powerFactor||0).toFixed(2));

  sB('bV',d.voltage||0,260);sB('bI',d.current||0,100);sB('bP',d.power||0,23000);
  sB('bE',d.energy||0,50000);sB('bF',d.frequency||0,65);sB('bPF',d.powerFactor||0,1);

  const al=d.alarmState||d.alarm||0;
  const pwrCard=$('pwrCard');if(pwrCard)pwrCard.classList.toggle('alarm-card',!!al);
  const alarmTag=$('alarmTag');if(alarmTag)alarmTag.classList.toggle('show',!!al);

  const rs=d.relayState!==undefined?d.relayState:(d.relay||0);
  const on=rs===1;
  const rw=d.online&&rs===0&&(d.power||0)>RELAY_WARN_W&&(Date.now()-(relayOffAt[d.id]||Date.now()))>SUPERFRAME_MS;
  sT('relayState',on?'ON':'OFF');
  const rSV=$('relayState');if(rSV)rSV.className='relay-state-val '+(on?'on':'off')+' M';
  const rT=$('relayToggle');if(rT)rT.checked=on;
  const rWT=$('relayWarnTag');if(rWT)rWT.classList.toggle('show',rw);
  const rSB=$('relayStateBox');if(rSB)rSB.classList.toggle('warn',rw);
  const rm=d.relayMode||0;
  sT('relayMode',rm===1?'SCHEDULED':'MANUAL');

  if(!d.pending&&pendingCmd[d.id]!==undefined)delete pendingCmd[d.id];
  const pend=d.pending||false;
  const cmdType=pendingCmd[d.id]||null;
  const cb=$('commitBanner');if(cb)cb.classList.toggle('show',pend&&d.online!==false);
  const rt=$('relayTabs');if(rt)rt.classList.toggle('frozen',pend);
  const ri=$('relayInner');if(ri)ri.classList.toggle('frozen',pend);

  const off=!d.online;
  const om=$('offlineMsg');if(om)om.classList.toggle('show',off);
  const rc=$('relayCtrl');if(rc)rc.style.display=off?'none':'block';
  const tc=$('thrCtrl');if(tc)tc.classList.toggle('frozen',off||pend);
  const nb=$('detNudge');
  if(nb){
    const nudgePend=cmdType==='nudge';
    nb.disabled=off||pend;
    nb.style.opacity=(off||pend)?'.3':'';
    nb.style.pointerEvents=(off||pend)?'none':'';
    clearTimeout(nb._t);
    if(nudgePend){nb.classList.add('nudged');nb.innerHTML=NUDGED_ICO+' Nudged';}
    else{nb.classList.remove('nudged');nb.innerHTML=NUDGE_ICO+' Nudge';}
  }

  const ss=d.schedState||0;
  const si=$('schedInfo');
  const schedActive=(rm===1&&ss>0&&d.hasSched);
  const relayTabs=$('relayTabs');
  const manTab=relayTabs?relayTabs.querySelector('[data-mode="manual"]'):null;
  if(manTab)manTab.classList.toggle('tab-disabled',schedActive);
  if(si){
    if(schedActive){
      si.className='sched-info M show';
      si.innerHTML=`<div><strong>Daily schedule</strong><span class="${SC[ss]||''}">${SL[ss]||''}</span><br>ON: ${d.schedStart||'--:--'} → OFF: ${d.schedEnd||'--:--'}</div><div class="sched-clear" onclick="doClear()">Clear</div>`;
      setRelayTab('schedule');
    }else{si.className='sched-info M';}
  }

  const cost=((d.energy||0)/1000)*costRate;
  sT('costVal',cost.toFixed(2));
  sT('costRate2','@ '+costRate.toFixed(2)+' / kWh');
  sT('costEnergy',(d.energy||0)+' Wh');
  const {cph,state:rateState}=getEnergyRateInfo(d.id,d.energy||0,d.online);
  const hint=$('costRateHint');
  if(hint){
    if(rateState==='waiting'){hint.textContent='Waiting for energy data...';hint.style.display='';}
    else if(rateState==='converging'){hint.textContent='Estimating — stabilizes over time';hint.style.display='';}
    else{hint.style.display='none';}
  }
  const extrapEls=['costHrVal','costDayVal','costWeekVal','costMonthVal'];
  const extrapMult=[1,24,24*7,24*30];
  extrapEls.forEach((id,i)=>{
    const el=$(id);if(!el)return;
    if(cph===null){el.textContent='--';el.classList.add('cost-val-pending');el.classList.remove('cost-val-unstable');}
    else{
      el.textContent=(cph*extrapMult[i]).toFixed(2);
      el.classList.remove('cost-val-pending');
      el.classList.toggle('cost-val-unstable',rateState==='converging');
    }
  });

  const infPan=$('inferencePanel');
  if(infPan){
    const clf=d.classifier;
    const knn=d.knn;
    const showKnn=gwHasKnn&&!!knn&&!!d.online;
    const showClf=gwHasClassifier&&!!clf&&!!clf.supported;
    infPan.style.display=(showKnn||showClf)?'':'none';
    const knnSec=$('infKnnSection');if(knnSec)knnSec.style.display=showKnn?'':'none';
    const clfSec=$('infClfSection');if(clfSec)clfSec.style.display=showClf?'':'none';
    const infDiv=$('infDivider');if(infDiv)infDiv.style.display=showClf?'':'none';

    if(showKnn){
      const state=knn.state||'UNIDENTIFIED';
      const lbl=$('knnDetLabel'),stEl=$('knnDetState'),hint=$('knnDetHint');
      if(state==='IDENTIFIED'&&knn.label){
        const dSq=knn.distSq||0;
        const[r,g,b]=knnStrengthRgb(dSq);
        const strength=Math.max(0,Math.min(1,1-dSq/0.30));
        const pct=Math.round(strength*100);
        const clr=`rgb(${r},${g},${b})`;
        if(lbl){lbl.textContent=knn.label;lbl.style.color=clr;}
        if(stEl){stEl.textContent='IDENTIFIED';stEl.style.cssText=`font-size:9px;background:rgba(${r},${g},${b},.12);color:${clr};border-color:rgba(${r},${g},${b},.30)`;}
        if(hint){hint.innerHTML=
          `<div style="display:flex;align-items:center;gap:6px;margin-bottom:2px">`+
          `<span style="font-size:9px;color:var(--txd);white-space:nowrap;font-family:'Fira Code',monospace">Match</span>`+
          `<div class="clf-bar-wrap" style="flex:1"><div class="clf-bar-fill" style="width:${pct}%;background:${clr}"></div></div>`+
          `<span class="clf-bar-pct M" style="color:${clr}">${pct}%</span>`+
          `</div>`+
          `<span style="font-size:9px;color:var(--txd);font-family:'Fira Code',monospace">dist2 = ${dSq.toFixed(3)}</span>`;}
      }else if(state==='SETTLING'){
        if(lbl){lbl.textContent='...';lbl.style.color='var(--wn)';}
        if(stEl){stEl.textContent='SETTLING';stEl.style.cssText='font-size:9px;background:rgba(255,159,67,.1);color:var(--wn);border-color:rgba(255,159,67,.3)';}
        if(hint)hint.textContent='Load settling -- classification will stabilize shortly';
      }else{
        if(lbl){lbl.textContent='Unidentified';lbl.style.color='var(--txd)';}
        if(stEl){stEl.textContent='IDLE';stEl.style.cssText='font-size:9px;background:transparent;color:var(--txd);border-color:var(--bd)';}
        if(hint)hint.textContent='No active load or appliance not in model';
      }
    }

    if(showClf){
      if(clf.pending){
        sT('clfClass','Analyzing...');
        const cc=$('clfClass');if(cc)cc.style.color='var(--txd)';
        const ce=$('clfConf');if(ce)ce.innerHTML='';
        const ct=$('clfTrans');if(ct)ct.style.display='none';
      }else{
        const name=CLF_NAMES[clf.classId]||'Unknown';
        const color=CLF_COLORS[clf.classId]||'#8892a4';
        sT('clfClass',name);
        const cc=$('clfClass');if(cc)cc.style.color=color;
        const ce=$('clfConf');
        const ct=$('clfTrans');
        const hint=$('clfHint');
        if(clf.classId===6){
          if(ce)ce.innerHTML='';
          if(ct)ct.style.display='none';
          if(hint)hint.textContent='This node is drawing negligible current - load inference is unavailable';
        }else{
          const conf=Math.min(7,Math.max(0,clf.confidence||0));
          const pct=Math.round(conf/7*100);
          if(ce){ce.innerHTML=`<div class="clf-bar-wrap"><div class="clf-bar-fill" style="width:${pct}%;background:${color}"></div></div><span class="clf-bar-pct M" style="color:${color}">${pct}%</span>`;}
          if(ct)ct.style.display=clf.transient?'':'none';
          if(hint)hint.textContent='Classification inspects I-V waveform characteristics. If the load class changes unexpectedly, check the connected load.';
        }
      }
    }
  }
}

/* -- Relay controls ---------------------------------------------- */
let _relayPanelSlot=null,_relayDialogAnchor=null;
function _positionRelayDialog(){
  if(window.innerWidth<=600)return;
  const dlg=$('relayDialog'),anchor=_relayDialogAnchor;
  if(!dlg||!anchor)return;
  const r=anchor.getBoundingClientRect();
  const dw=dlg.offsetWidth||360;
  const dh=dlg.offsetHeight||300;
  let left=r.left;
  let top=r.bottom+8;
  if(left+dw>window.innerWidth-8)left=window.innerWidth-dw-8;
  if(left<8)left=8;
  if(top+dh>window.innerHeight-8){
    const above=r.top-dh-8;
    top=above>=8?above:Math.max(8,window.innerHeight-dh-8);
  }
  dlg.style.cssText='margin:0;top:'+top+'px;left:'+left+'px';
}
function openRelayDialog(nid,e){
  e.stopPropagation();
  const panel=$('relayPanel'),body=$('relayDlgBody');
  if(!panel||!body)return;
  _relayPanelSlot={parent:panel.parentNode,next:panel.nextSibling};
  body.appendChild(panel);
  cNid=nid;
  const n=NC.find(x=>x.id===nid);
  if(n){updateDetail(n);const t=$('relayDlgTitle');if(t)t.textContent=n.label||('Node #'+nid);}
  const dlg=$('relayDialog');
  dlg.classList.remove('dlg-anchored');
  dlg.style.cssText='';
  dlg.showModal();
  _relayDialogAnchor=e.currentTarget;
  if(window.innerWidth>600){dlg.classList.add('dlg-anchored');_positionRelayDialog();}
}
function closeRelayDialog(){
  const panel=$('relayPanel');
  if(_relayPanelSlot&&panel){
    _relayPanelSlot.parent.insertBefore(panel,_relayPanelSlot.next);
    _relayPanelSlot=null;
  }
  _relayDialogAnchor=null;
  const dlg=$('relayDialog');
  if(dlg){dlg.classList.remove('dlg-anchored');dlg.style.cssText='';dlg.close();}
}

function setRelayTab(m){
  document.querySelectorAll('#relayTabs div').forEach(t=>t.classList.toggle('active',t.dataset.mode===m));
  const tabManual=$('tabManual');if(tabManual)tabManual.classList.toggle('active',m==='manual');
  const tabSchedule=$('tabSchedule');if(tabSchedule)tabSchedule.classList.toggle('active',m==='schedule');
  if(_relayDialogAnchor)_positionRelayDialog();
}
function freezeAllCmds(){
  const cb=$('commitBanner');if(cb)cb.classList.add('show');
  const rt=$('relayTabs');if(rt)rt.classList.add('frozen');
  const ri=$('relayInner');if(ri)ri.classList.add('frozen');
  const tc=$('thrCtrl');if(tc)tc.classList.add('frozen');
  const nb=$('detNudge');
  if(nb){nb.disabled=true;nb.style.opacity='.3';nb.style.pointerEvents='none';}
}
function doManual(c){
  pendingCmd[cNid]='relay_manual';
  wsSend({cmd:'relay_manual',node:cNid,state:c?1:0});
  freezeAllCmds();
  const ni=NC.findIndex(x=>x.id===cNid);
  if(ni>=0){NC[ni].relayState=c?1:0;const pg=$('pg-overview');if(pg&&pg.classList.contains('active'))renderGrid(NC);}
}
function doSchedule(){
  const sv=$('schedStart').value,ev=$('schedEnd').value;
  if(!sv||!ev){showToast('Set both start and end times.','warn');return;}
  const[sh,sm]=sv.split(':').map(Number),[eh,em]=ev.split(':').map(Number);
  pendingCmd[cNid]='relay_sched';
  wsSend({cmd:'relay_schedule',node:cNid,startH:sh,startM:sm,endH:eh,endM:em});
  freezeAllCmds();
}
function doClear(){pendingCmd[cNid]='relay_sched';wsSend({cmd:'relay_clear',node:cNid});freezeAllCmds();}
function doSetThreshold(){
  const v=parseInt($('thrInput').value);
  if(!v||v<1||v>23000){showToast('Enter a value between 1 and 23000 W.','warn');return;}
  pendingCmd[cNid]='threshold';
  wsSend({cmd:'set_threshold',node:cNid,watts:v});
  freezeAllCmds();
}

/* -- Key regen --------------------------------------------------- */
async function execKeyRegen(){
  const st=$('provStatus');
  if(st){st.style.color='var(--txd)';st.textContent='Regenerating...';}
  try{
    const r=await fetch('/api/keygen',{method:'POST'});
    const d=await r.json();
    if(st){st.style.color=d.ok?'var(--ac)':'var(--dg)';st.textContent=d.ok?'New key generated':'Failed';}
  }catch(e){
    if(st){st.style.color='var(--dg)';st.textContent='Request failed';}
  }
  setTimeout(()=>{if(st)st.textContent='';},3000);
}

/* -- Nudge ------------------------------------------------------- */
function doNudge(nodeId,ev){
  if(ev)ev.stopPropagation();
  pendingCmd[nodeId]='nudge';
  wsSend({cmd:'nudge',node:nodeId});
  document.querySelectorAll(`.nudge-btn[data-nid="${nodeId}"]`).forEach(btn=>{
    btn.classList.add('nudged');
    btn.innerHTML=NUDGED_ICO+' Nudged';
    clearTimeout(btn._t);btn._t=setTimeout(()=>{btn.classList.remove('nudged');btn.innerHTML=NUDGE_ICO+' Nudge';},3000);
  });
  if(cNid===nodeId){
    const nb=$('detNudge');
    if(nb){nb.classList.add('nudged');nb.innerHTML=NUDGED_ICO+' Nudged';nb.disabled=true;nb.style.opacity='.3';nb.style.pointerEvents='none';}
  }
}

/* -- Cost rate --------------------------------------------------- */
function setCostRate(){
  const v=parseFloat($('costRateIn').value);
  if(!v||v<=0){showToast('Enter a valid cost rate.','warn');return;}
  costRate=v;localStorage.setItem('pt-costRate',v);
  const btn=$('costSetBtn');
  const prev=btn.textContent;
  btn.textContent='✓ Saved';btn.style.background='rgba(0,229,160,.15)';btn.style.color='var(--ac)';
  btn.disabled=true;clearTimeout(btn._t);
  btn._t=setTimeout(()=>{btn.textContent=prev;btn.style.background='var(--ac)';btn.style.color='var(--bg)';btn.disabled=false;},2000);
}

/* -- Rename ------------------------------------------------------- */
function startRename(){
  const el=$('detLabel');if(!el)return;
  const cur=el.textContent;
  const inp=document.createElement('input');
  inp.className='rename-input';inp.value=cur;inp.maxLength=29;
  el.replaceWith(inp);inp.focus();inp.select();
  let done=false;
  function commit(){
    if(done)return;done=true;
    const nv=inp.value.trim();
    if(nv&&nv!==cur)wsSend({cmd:'rename',node:cNid,name:nv});
    const h2=document.createElement('h2');h2.id='detLabel';h2.textContent=nv||cur;
    h2.onclick=startRename;h2.title='Click to rename';h2.tabIndex=0;
    h2.onkeydown=e=>{if(e.key==='Enter'||e.key===' ')startRename();};
    if(inp.isConnected)inp.replaceWith(h2);
    else{const p=document.querySelector('.det-hdr-l');if(p)p.prepend(h2);}
  }
  inp.addEventListener('keydown',e=>{if(e.key==='Enter'){e.preventDefault();commit();}if(e.key==='Escape'){inp.value=cur;commit();}});
  inp.addEventListener('blur',commit);
}

/* -- Chart ------------------------------------------------------- */
function getCS(v){return getComputedStyle(document.documentElement).getPropertyValue(v).trim();}

function initChart(){
  if(chart)chart.destroy();
  const mc=MC[cMet];
  chart=new Chart($('chartCanvas').getContext('2d'),{
    type:'line',
    data:{labels:[],datasets:[{label:mc.l,data:[],borderColor:mc.c,backgroundColor:mc.c+'18',fill:true,tension:.35,borderWidth:2,pointRadius:0,pointHitRadius:10}]},
    options:{
      responsive:true,maintainAspectRatio:false,animation:{duration:300},
      interaction:{intersect:false,mode:'index'},
      plugins:{
        legend:{display:false},
        tooltip:{
          backgroundColor:getCS('--chart-tip-bg'),borderColor:getCS('--chart-tip-bd'),borderWidth:1,
          titleColor:getCS('--chart-tx'),bodyColor:getCS('--chart-tx'),
          titleFont:{family:"'Fira Code'",size:10},bodyFont:{family:"'Fira Code'",size:11},
          padding:8,cornerRadius:5
        }
      },
      scales:{
        x:{grid:{color:getCS('--chart-grid')},ticks:{color:getCS('--chart-tx'),font:{family:"'Fira Code'",size:9},maxTicksLimit:8}},
        y:{beginAtZero:true,afterDataLimits(s){const d=s.chart.data.datasets[0].data;if(d&&d.length){const pk=Math.max(...d.filter(v=>v!=null));if(pk>0)s.max=pk*1.2;}},grid:{color:getCS('--chart-grid')},ticks:{color:getCS('--chart-tx'),font:{family:"'Fira Code'",size:9}}}
      }
    }
  });
}

function swChart(m){
  cMet=m;
  document.querySelectorAll('[data-m]').forEach(b=>{
    const active=b.dataset.m===m;
    b.style.borderColor=active?'var(--ac)':'';
    b.style.color=active?'var(--ac)':'';
    b.style.background=active?'rgba(0,229,160,.08)':'';
    b.setAttribute('aria-pressed',String(active));
  });
  const mc=MC[m];
  chart.data.datasets[0].label=mc.l;
  chart.data.datasets[0].borderColor=mc.c;
  chart.data.datasets[0].backgroundColor=mc.c+'18';
  chart.data.datasets[0].data=[];chart.data.labels=[];
  chart.update('none');fetchHistory(cNid);
}

function addChartPoint(n){
  if(!chart)return;
  const mc=MC[cMet];
  const v=n[mc.n]!==undefined?n[mc.n]:n[mc.k];
  if(v===undefined)return;
  const ds=chart.data.datasets[0];
  ds.data.push(v);chart.data.labels.push('');
  if(ds.data.length>120){ds.data.shift();chart.data.labels.shift();}
  const sfS=Math.round(SUPERFRAME_MS/1000);
  chart.data.labels=ds.data.map((_,i)=>{const s=(ds.data.length-1-i)*sfS;return s>60?Math.floor(s/60)+'m':s+'s';});
  chart.update('none');
  pushSparkPoint(n);updateSparklines();
}

async function fetchHistory(id){
  try{
    const r=await fetch('/api/node/'+id+'/history');
    const d=await r.json();
    if(!d.length||!chart)return;
    const mc=MC[cMet];
    const sfS=Math.round(SUPERFRAME_MS/1000);
    chart.data.labels=d.map((_,i)=>{const s=(d.length-1-i)*sfS;return s>60?Math.floor(s/60)+'m':s+'s';});
    chart.data.datasets[0].data=d.map(x=>x[mc.k]);
    chart.update('none');
    loadSparkFromHistory(d);updateSparklines();
  }catch(e){}
}

async function fetchSys(){
  try{
    const r=await fetch('/api/status');const d=await r.json();
    sT('gwUptime',fU(d.uptime));
    sT('gwHeap',(d.freeHeap/1024).toFixed(0)+' KB');
    sT('gwNodes',(d.nodeCount||0)+'/'+(d.maxNodes||8));
    if(d.timeSet!==undefined)gwTimeSet=d.timeSet;
    if(d.time)gwTime=d.time;
    if(d.ntpSynced!==undefined)gwNtpSynced=d.ntpSynced;
    updGwTime();
    if(d.version){sT('fwVer',d.version);sT('fwVerFt',d.version);}
    updNetPanel(d);
  }catch(e){}
}

function updNetPanel(d){
  const apMode=d.wifiMode==='ap'||!d.wifiRSSI;
  sT('gwSSID',d.ssid||'--');
  const badge=$('gwModeBadge');
  if(badge){
    badge.textContent=apMode?'AP':'STA';
    badge.className='panel-badge M '+(apMode?'wn':'ac');
  }
  const icon=$('gwNetIcon');
  if(icon){
    icon.className='gw-net-icon '+(apMode?'ap':'sta');
    if(apMode){
      icon.innerHTML='<svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="var(--wn)" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"><path d="M1.42 9a16 16 0 0 1 21.16 0"/><path d="M5 12.55a11 11 0 0 1 14.08 0"/><path d="M8.53 16.11a6 6 0 0 1 6.95 0"/><line x1="12" y1="20" x2="12.01" y2="20" stroke-width="2.5"/></svg>';
    }else{
      const rssi=d.wifiRSSI||0;
      icon.innerHTML=(rssi>=-55?'<svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"><path d="M1.42 9a16 16 0 0 1 21.16 0"/><path d="M5 12.55a11 11 0 0 1 14.08 0"/><path d="M8.53 16.11a6 6 0 0 1 6.95 0"/><line x1="12" y1="20" x2="12.01" y2="20" stroke-width="2.5"/></svg>':rssi>=-65?'<svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"><path d="M5 12.55a11 11 0 0 1 14.08 0"/><path d="M8.53 16.11a6 6 0 0 1 6.95 0"/><line x1="12" y1="20" x2="12.01" y2="20" stroke-width="2.5"/></svg>':rssi>=-75?'<svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"><path d="M8.53 16.11a6 6 0 0 1 6.95 0"/><line x1="12" y1="20" x2="12.01" y2="20" stroke-width="2.5"/></svg>':'<svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"><line x1="12" y1="20" x2="12.01" y2="20" stroke-width="2.5"/></svg>');
    }
  }
  sT('gwIP',d.ip||'--');
}

/* -- WiFi config ----------------------------------------------- */
let wifiSelectedSSID='';
let wifiPollIv=null;
let wifiDrawerOpen=false;
let gwCtrlDrawerOpen=false;
let wifiCachedStaticIp='';
let wifiStaticSaved=false;

function isValidIp(s){
  const p=s.split('.');
  if(p.length!==4)return false;
  return p.every(o=>o!==''&&/^\d+$/.test(o)&&+o>=0&&+o<=255);
}

function isValidSubnet(s){
  if(!isValidIp(s))return false;
  const n=s.split('.').map(Number).reduce((a,b)=>(a<<8)|b,0)>>>0;
  const inv=(~n)>>>0;
  return(inv&(inv+1))===0;
}

function wifiSetFieldErr(id,hasErr,msg){
  const inp=$(id);
  const hint=$(id+'Err');
  if(inp)inp.classList.toggle('err',hasErr);
  if(hint){hint.textContent=hasErr?msg:'';hint.style.display=hasErr?'':'none';}
}

function wifiValidateStaticFields(){
  wifiStaticSaved=false;
  const ip=$('wifiStaticIp').value.trim();
  const gw=$('wifiStaticGw').value.trim();
  const sn=$('wifiStaticSn').value.trim();
  const dns=$('wifiStaticDns').value.trim();

  const ipOk=isValidIp(ip);
  const gwOk=isValidIp(gw);
  const snOk=isValidSubnet(sn);
  const dnsOk=!dns||isValidIp(dns);

  wifiSetFieldErr('wifiStaticIp',ip&&!ipOk,'Invalid IP address');
  wifiSetFieldErr('wifiStaticGw',gw&&!gwOk,'Invalid gateway address');
  wifiSetFieldErr('wifiStaticSn',sn&&!snOk,'Invalid subnet mask');
  wifiSetFieldErr('wifiStaticDns',dns&&!dnsOk,'Invalid DNS address');

  const gwWarn=$('wifiStaticGwWarn');
  if(ipOk&&gwOk&&snOk){
    const ipP=ip.split('.').map(Number);
    const gwP=gw.split('.').map(Number);
    const snP=sn.split('.').map(Number);
    const same=ipP.every((b,i)=>(b&snP[i])===(gwP[i]&snP[i]));
    if(gwWarn){gwWarn.textContent=same?'':'Gateway not on same subnet as IP';gwWarn.style.display=same?'none':'';}
  }else{
    if(gwWarn){gwWarn.textContent='';gwWarn.style.display='none';}
  }

  const saveErr=$('wifiStaticSaveErr');
  if(saveErr){saveErr.textContent='';saveErr.style.display='none';}

  const allOk=ip&&gw&&sn&&ipOk&&gwOk&&snOk&&dnsOk;
  const ssb=$('wifiStaticSaveBtn');if(ssb)ssb.disabled=!allOk;
  wifiValidateConnect();
}

/* -- Screen routing -------------------------------------------- */
function wifiShowScan(){
  $('wifiScr-scan').style.display='';
  $('wifiScr-connecting').style.display='none';
  $('wifiScr-result').style.display='none';
}
function wifiShowConnecting(ssid){
  $('wifiScr-scan').style.display='none';
  $('wifiScr-connecting').style.display='';
  $('wifiScr-result').style.display='none';
  sT('wifiConnTitle','Connecting...');
  sT('wifiConnSub',ssid);
}
function wifiShowResult(ok,ip,ssid){
  $('wifiScr-scan').style.display='none';
  $('wifiScr-connecting').style.display='none';
  $('wifiScr-result').style.display='';
  const ico=$('wifiResultIcon');
  if(ok){
    ico.style.borderColor='var(--ac)';
    ico.style.background='rgba(0,229,160,.1)';
    ico.style.color='var(--ac)';
    ico.innerHTML='<svg width="22" height="22" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round"><polyline points="20 6 9 17 4 12"/></svg>';
  }else{
    ico.style.borderColor='var(--dg)';
    ico.style.background='rgba(255,56,96,.1)';
    ico.style.color='var(--dg)';
    ico.innerHTML='<svg width="22" height="22" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round"><line x1="18" y1="6" x2="6" y2="18"/><line x1="6" y1="6" x2="18" y2="18"/></svg>';
  }
  sT('wifiResultTitle',ok?'Connected!':'Failed');
  const rsEl=$('wifiResultSub');
  if(rsEl)rsEl.innerHTML=ok
    ?`Gateway joined <strong>${esc(ssid)}</strong>.<br>IP: <span style="color:var(--ac)">${esc(ip)}</span>`
    :`Could not connect to <strong>${esc(ssid)}</strong>.<br>Check password and try again.`;
  const backBtn=$('wifiBackBtn');
  if(backBtn)backBtn.onclick=ok?()=>wifiShowScan():()=>wifiShowScan();
}

/* -- Scan ------------------------------------------------------- */
function wifiDoScan(){
  const btn=$('wifiScanBtn');
  const list=$('wifiNetList');
  btn.classList.add('spinning');
  btn.disabled=true;
  list.innerHTML='<div style="text-align:center;padding:18px 0;color:var(--txd);font-size:11px;font-family:\'Fira Code\',monospace">Scanning...</div>';
  let scanTries=0;
  function poll(){
    fetch('/api/scan')
      .then(r=>r.json())
      .then(d=>{
        if(d.scanning&&++scanTries<20){setTimeout(poll,800);return;}
        btn.classList.remove('spinning');
        btn.disabled=false;
        wifiRenderNetworks(d.networks||[]);
      })
      .catch(()=>{
        list.innerHTML='<div style="text-align:center;padding:18px 0;color:var(--dg);font-size:11px;font-family:\'Fira Code\',monospace">Scan failed -- check connection</div>';
        btn.classList.remove('spinning');
        btn.disabled=false;
      });
  }
  poll();
}

function wifiRenderNetworks(nets){
  const list=$('wifiNetList');
  if(!nets.length){
    list.innerHTML='<div style="text-align:center;padding:18px 0;color:var(--txd);font-size:11px;font-family:\'Fira Code\',monospace">No networks found</div>';
    return;
  }
  const lockSvg='<svg width="12" height="12" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><rect x="3" y="11" width="18" height="11" rx="2"/><path d="M7 11V7a5 5 0 0 1 10 0v4"/></svg>';
  const openSvg='<svg width="12" height="12" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M1.42 9a16 16 0 0 1 21.16 0"/><path d="M5 12.55a11 11 0 0 1 14.08 0"/><path d="M8.53 16.11a6 6 0 0 1 6.95 0"/><line x1="12" y1="20" x2="12.01" y2="20" stroke-width="2.5"/></svg>';
  list.innerHTML='';
  nets.sort((a,b)=>b.rssi-a.rssi).forEach((n,i)=>{
    const bars=rssiToBars(n.rssi);
    const el=document.createElement('div');
    el.className='wifi-net-item';
    el.setAttribute('role','option');
    el.style.animationDelay=(i*0.04)+'s';
    el.style.animation='slideIn .3s ease both';
    const sigSvg=bars>=4?'<svg width="15" height="15" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"><path d="M1.42 9a16 16 0 0 1 21.16 0"/><path d="M5 12.55a11 11 0 0 1 14.08 0"/><path d="M8.53 16.11a6 6 0 0 1 6.95 0"/><line x1="12" y1="20" x2="12.01" y2="20" stroke-width="2.5"/></svg>':bars>=3?'<svg width="15" height="15" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"><path d="M5 12.55a11 11 0 0 1 14.08 0"/><path d="M8.53 16.11a6 6 0 0 1 6.95 0"/><line x1="12" y1="20" x2="12.01" y2="20" stroke-width="2.5"/></svg>':bars>=2?'<svg width="15" height="15" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"><path d="M8.53 16.11a6 6 0 0 1 6.95 0"/><line x1="12" y1="20" x2="12.01" y2="20" stroke-width="2.5"/></svg>':'<svg width="15" height="15" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"><line x1="12" y1="20" x2="12.01" y2="20" stroke-width="2.5"/></svg>';
    el.innerHTML=
      '<span style="color:var(--txd);display:flex;align-items:center">'+(n.secure?lockSvg:openSvg)+'</span>'+
      '<div class="wifi-net-info">'+
        '<div class="wifi-net-name">'+esc(n.ssid)+'</div>'+
        '<div class="wifi-net-meta">'+n.rssi+' dBm · '+(n.secure?'WPA2':'Open')+'</div>'+
      '</div>'+sigSvg;
    el.addEventListener('click',()=>wifiSelectNetwork(n.ssid,n.secure,el));
    list.appendChild(el);
  });
}

function wifiSelectNetwork(ssid,secure,el){
  document.querySelectorAll('.wifi-net-item').forEach(i=>i.classList.remove('selected'));
  el.classList.add('selected');
  wifiSelectedSSID=ssid;
  $('wifiManualCb').checked=false;
  wifiToggleManual();
  $('wifiCredSection').style.display='block';
  const pw=$('wifiPwInput');
  if(!secure){pw.value='';pw.placeholder='No password required';pw.disabled=true;}
  else{pw.placeholder='Enter password...';pw.disabled=false;pw.focus();}
  wifiValidateConnect();
}

/* -- Manual SSID toggle ----------------------------------------- */
function wifiToggleManual(){
  const manual=$('wifiManualCb').checked;
  $('wifiManualInput').style.display=manual?'block':'none';
  if(manual){
    document.querySelectorAll('.wifi-net-item').forEach(i=>i.classList.remove('selected'));
    wifiSelectedSSID='';
    $('wifiCredSection').style.display='block';
    const pw=$('wifiPwInput');
    pw.placeholder='Enter password...';pw.disabled=false;
    $('wifiManualSSID').focus();
  }else{
    if(!wifiSelectedSSID)$('wifiCredSection').style.display='none';
  }
  wifiValidateConnect();
}

function wifiValidateConnect(){
  const manual=$('wifiManualCb').checked;
  const ssid=manual?$('wifiManualSSID').value.trim():wifiSelectedSSID;
  let staticOk=true;
  if($('wifiStaticIpCb').checked){
    const ip=$('wifiStaticIp').value.trim();
    const gw=$('wifiStaticGw').value.trim();
    const sn=$('wifiStaticSn').value.trim();
    staticOk=wifiStaticSaved&&isValidIp(ip)&&isValidIp(gw)&&isValidSubnet(sn);
  }
  const btn=$('wifiConnectBtn');
  if(btn){btn.disabled=!ssid||!staticOk;btn.title=!staticOk?'Save valid static IP settings first':'';}
}

/* -- Password eye ----------------------------------------------- */
function wifiTogglePw(){
  const inp=$('wifiPwInput');
  const ico=$('wifiEyeIco');
  const show=inp.type==='password';
  inp.type=show?'text':'password';
  if(ico)ico.innerHTML=show
    ?'<path d="M17.94 17.94A10.07 10.07 0 0 1 12 20c-7 0-11-8-11-8a18.45 18.45 0 0 1 5.06-5.94"/><path d="M9.9 4.24A9.12 9.12 0 0 1 12 4c7 0 11 8 11 8a18.5 18.5 0 0 1-2.16 3.19"/><line x1="1" y1="1" x2="23" y2="23"/>'
    :'<path d="M1 12s4-8 11-8 11 8 11 8-4 8-11 8-11-8-11-8z"/><circle cx="12" cy="12" r="3"/>';
}

/* -- Connect ---------------------------------------------------- */
function wifiDoConnect(){
  const manual=$('wifiManualCb').checked;
  const ssid=manual?$('wifiManualSSID').value.trim():wifiSelectedSSID;
  if(!ssid)return;
  const pwEl=$('wifiPwInput');
  const pw=pwEl.value;
  pwEl.value='';
  const badge=$('gwModeBadge');
  const wasInSta=badge&&badge.textContent.trim()==='STA';
  wifiShowConnecting(ssid);
  const ctrl=new AbortController();
  const abortT=setTimeout(()=>ctrl.abort(),5000);
  fetch('/api/connect',{
    method:'POST',
    headers:{'Content-Type':'application/x-www-form-urlencoded'},
    body:'ssid='+encodeURIComponent(ssid)+'&password='+encodeURIComponent(pw),
    signal:ctrl.signal
  })
  .then(r=>{clearTimeout(abortT);return r.json();})
  .then(d=>{
    if(d.status==='connecting'){wifiPollStatus(ssid,wasInSta);}
    else{wifiShowResult(false,'',ssid);}
  })
  .catch(()=>{clearTimeout(abortT);wifiShowApDropped(ssid,wasInSta);});
}

function wifiPollStatus(ssid,wasInSta){
  let tries=0;
  let failStreak=0;
  clearInterval(wifiPollIv);
  wifiPollIv=setInterval(()=>{
    fetch('/api/wifistatus')
      .then(r=>r.json())
      .then(d=>{
        failStreak=0;
        if(d.connected){clearInterval(wifiPollIv);wifiShowResult(true,d.ip,ssid);fetchSys();}
        else if(tries++>25){clearInterval(wifiPollIv);wifiShowResult(false,'',ssid);}
      })
      .catch(()=>{
        tries++;
        if(++failStreak>=4){clearInterval(wifiPollIv);wifiShowApDropped(ssid,wasInSta);}
      });
  },800);
}

function wifiShowApDropped(ssid,isNetworkSwitch){
  if(socket)socket.close();
  $('wifiScr-scan').style.display='none';
  $('wifiScr-connecting').style.display='none';
  $('wifiScr-result').style.display='';
  const ico=$('wifiResultIcon');
  ico.style.borderColor='var(--wn)';
  ico.style.background='rgba(255,159,67,.1)';
  ico.style.color='var(--wn)';
  ico.innerHTML='<svg width="22" height="22" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round"><path d="M5 12.55a11 11 0 0 1 14.08 0"/><path d="M1.42 9a16 16 0 0 1 21.16 0"/><path d="M8.53 16.11a6 6 0 0 1 6.95 0"/><line x1="12" y1="20" x2="12.01" y2="20" stroke-width="2.5"/></svg>';
  sT('wifiResultTitle','Gateway Connected');
  const ipHint=wifiCachedStaticIp
    ?`<br>Static IP: <a href="http://${esc(wifiCachedStaticIp)}" target="_blank" style="color:var(--ac);font-family:'Fira Code',monospace;font-size:12px">${esc(wifiCachedStaticIp)}</a>`
    :'';
  const body=isNetworkSwitch
    ?`Gateway is switching to <strong>${esc(ssid)}</strong>.<br>Connect this device to <strong>${esc(ssid)}</strong>, then open:<br>`
    :`AP was turned off -- gateway joined <strong>${esc(ssid)}</strong>.<br>Connect this device to <strong>${esc(ssid)}</strong>, then open:<br>`;
  const rsEl=$('wifiResultSub');
  if(rsEl)rsEl.innerHTML=body+
    `<a href="http://telemeter.local" target="_blank" style="color:var(--ac);font-family:'Fira Code',monospace;font-size:12px">http://telemeter.local</a>`+
    ipHint+
    `<br><br><span style="color:var(--txd);font-size:10px">If unreachable, hold the BOOT button and power-cycle to restore AP mode.</span>`;
  const backBtn=$('wifiBackBtn');
  if(backBtn){backBtn.innerHTML='Close';backBtn.onclick=()=>wifiShowScan();}
}

function wifiCancelConnect(){
  clearInterval(wifiPollIv);
  fetch('/api/disconnect').catch(()=>{});
  wifiShowScan();
}

/* -- Static IP -------------------------------------------------- */
function wifiLoadStaticIp(){
  fetch('/api/staticip')
    .then(r=>r.json())
    .then(d=>{
      $('wifiStaticIpCb').checked=d.enabled;
      $('wifiStaticIp').value=d.ip||'';
      $('wifiStaticGw').value=d.gateway||'';
      $('wifiStaticSn').value=d.subnet||'';
      $('wifiStaticDns').value=d.dns||'';
      wifiCachedStaticIp=d.enabled?d.ip:'';
      wifiStaticSaved=true;
      wifiToggleStaticIp();
    })
    .catch(()=>{});
}

function wifiToggleStaticIp(){
  const enabled=$('wifiStaticIpCb').checked;
  const sec=$('wifiStaticIpSection');if(sec)sec.style.display=enabled?'':'none';
  if(enabled&&$('wifiStaticIp')&&!$('wifiStaticIp').value)$('wifiStaticIp').focus();
  wifiValidateConnect();
}

function wifiSaveStaticIp(){
  const ip=$('wifiStaticIp').value.trim();
  const gw=$('wifiStaticGw').value.trim();
  const sn=$('wifiStaticSn').value.trim();
  const dns=$('wifiStaticDns').value.trim();
  fetch('/api/staticip',{
    method:'POST',
    headers:{'Content-Type':'application/x-www-form-urlencoded'},
    body:'ip='+encodeURIComponent(ip)+'&gateway='+encodeURIComponent(gw)+'&subnet='+encodeURIComponent(sn)+'&dns='+encodeURIComponent(dns)
  })
  .then(r=>r.json())
  .then(d=>{
    const saveErr=$('wifiStaticSaveErr');
    if(d.ok){
      wifiCachedStaticIp=ip;
      wifiStaticSaved=true;
      if(saveErr){saveErr.textContent='';saveErr.style.display='none';}
      const btn=$('wifiStaticSaveBtn');
      const prev=btn.textContent;
      btn.textContent='Saved';btn.disabled=true;
      setTimeout(()=>{btn.textContent=prev;btn.disabled=false;},2000);
      wifiValidateConnect();
    }else{
      if(saveErr){saveErr.textContent='Rejected by device -- check address format.';saveErr.style.display='';}
    }
  })
  .catch(()=>{
    const saveErr=$('wifiStaticSaveErr');
    if(saveErr){saveErr.textContent='Save failed -- check connection.';saveErr.style.display='';}
  });
}

function wifiClearStaticIp(){
  fetch('/api/staticip/clear')
    .then(r=>r.json())
    .then(()=>{
      wifiCachedStaticIp='';
      $('wifiStaticIpCb').checked=false;
      $('wifiStaticIp').value='';
      $('wifiStaticGw').value='';
      $('wifiStaticSn').value='';
      $('wifiStaticDns').value='';
      ['wifiStaticIp','wifiStaticGw','wifiStaticSn','wifiStaticDns'].forEach(id=>{
        const el=$(id);if(el)el.classList.remove('err');
        const h=$(id+'Err');if(h){h.textContent='';h.style.display='none';}
      });
      const gw=$('wifiStaticGwWarn');if(gw){gw.textContent='';gw.style.display='none';}
      const se=$('wifiStaticSaveErr');if(se){se.textContent='';se.style.display='none';}
      wifiStaticSaved=true;
      wifiToggleStaticIp();
    })
    .catch(()=>{});
}

/* -- Forget ----------------------------------------------------- */
function wifiDoForget(){
  fetch('/api/forget')
    .then(()=>{
      wifiSelectedSSID='';
      const nl=$('wifiNetList');if(nl)nl.innerHTML='<div style="text-align:center;padding:18px 0;color:var(--txd);font-size:11px;font-family:\'Fira Code\',monospace">Credentials cleared. Scan to reconnect.</div>';
      const cs=$('wifiCredSection');if(cs)cs.style.display='none';
      $('wifiManualCb').checked=false;
      wifiToggleManual();
      setTimeout(()=>location.reload(),1500);
    })
    .catch(()=>{});
}

/* -- Features ---------------------------------------------------- */
let gwFeatures={};
// True unless firmware explicitly reports classifier:false.
// Stays true when /api/features is absent (old firmware) so per-node clf.supported acts as the gate.
let gwHasClassifier=true;
let gwHasKnn=false;
let knnPollIv=null;
let knnTrainingNodes=new Set();
let knnCountdownIv=null;
let knnCountdownEndMs=0;

async function fetchFeatures(){
  try{
    const r=await fetch('/api/features');
    const d=await r.json();
    gwFeatures=d;
    const enc=!!d.encryption;
    const tt=!!d['transport-test'];
    const ps=$('provSection');if(ps)ps.style.display=enc?'':'none';
    const ts=$('transportTestSection');if(ts)ts.style.display=tt?'':'none';
    if(tt)ttPopulateNodeSel();
    if(d.classifier===false){
      gwHasClassifier=false;
    }
    const knn=!!d.knn;
    gwHasKnn=knn;
    const ks=$('knnSection');if(ks)ks.style.display=knn?'block':'none';
    if(knn){applyKnnMgmt();knnPopulateNodeSel();fetchKnnStatus();}
    else{const dtp=$('detailTrainPanel');if(dtp)dtp.style.display='none';}
  }catch(e){}
}

/* -- Transport Test ---------------------------------------------- */
function ttPopulateNodeSel(){
  const sel=$('ttNodeSel');if(!sel)return;
  while(sel.options.length>1)sel.remove(1);
  NC.forEach(n=>{
    if(!n.online)return;
    const opt=document.createElement('option');
    opt.value=n.id;
    opt.textContent='Node '+n.id+' -- '+esc(n.label);
    sel.appendChild(opt);
  });
}

function doTransportTest(){
  const btn=$('ttRunBtn'),st=$('ttStatus'),res=$('ttResult');
  const nodeId=parseInt(($('ttNodeSel')&&$('ttNodeSel').value)||'0');
  const scen=($('ttScenSel')&&$('ttScenSel').value)||'echo';
  if(!nodeId){
    if(st){st.style.color='var(--wn)';st.textContent='Select a node first';}
    return;
  }
  if(btn)btn.disabled=true;
  if(st){st.style.color='var(--txd)';st.textContent='Sending grant...';}
  if(res)res.style.display='none';

  let cmd_scen=scen,cmd_size;
  if(scen.startsWith('boundary_')){cmd_scen='boundary';cmd_size=parseInt(scen.split('_')[1]);}

  const payload={cmd:'transport_test',node:nodeId,scenario:cmd_scen};
  if(cmd_size!==undefined)payload.size=cmd_size;
  wsSend(payload);
}

function onTransportTestAck(m){
  const btn=$('ttRunBtn'),st=$('ttStatus');
  if(!m.success){
    if(st){st.style.color='var(--dg)';st.textContent='Failed: '+(m.reason||'error');}
    if(btn)btn.disabled=false;
    return;
  }
  if(st){st.style.color='var(--ac)';st.textContent='Grant sent ('+m.size+' B, '+m.scenario+') -- waiting...';}
}

function onBulkComplete(m){
  const btn=$('ttRunBtn'),st=$('ttStatus'),res=$('ttResult');
  const ok=m.type==='bulk_complete';
  if(st){st.style.color=ok?'var(--ac)':'var(--dg)';st.textContent=ok?'Complete':'Failed';}
  if(btn)btn.disabled=false;
  if(!res)return;
  if(!ok){
    res.style.display='block';
    res.style.borderColor='rgba(224,82,82,.35)';
    res.innerHTML='<span style="color:var(--dg)">FAILED</span>  '+esc(m.reason||'unknown');
    return;
  }
  const pdr=m.pdrTotal>0?'PDR: '+m.pdrAcked+'/'+m.pdrTotal+' ('+(m.pdrPct||0).toFixed(1)+'%)':'';
  const crcLine=m.dir===0
    ?'CRC32: '+(m.crc32ok?'<span style="color:var(--ac)">PASS</span>':'<span style="color:var(--dg)">FAIL</span>')
    :'CRC32: '+esc(m.crc32||'--');
  res.style.display='block';
  res.style.borderColor='rgba(0,229,160,.2)';
  res.innerHTML=[
    '<span style="color:var(--ac)">COMPLETE</span>',
    'Node '+m.node+'  &bull;  '+m.totalLen+' B  &bull;  '+m.durationMs+' ms',
    'Frags: '+m.fragsAcked+'/'+m.fragsSent+' ACKed',
    crcLine,pdr
  ].filter(Boolean).join('<br>');
}

/* -- KNN Appliance Inference ------------------------------------- */
function knnPopulateNodeSel(){
  const sel=$('knnTrainNode');if(!sel)return;
  while(sel.options.length>1)sel.remove(1);
  NC.forEach(n=>{
    if(!n.online)return;
    const opt=document.createElement('option');
    opt.value=n.id;opt.textContent='Node '+n.id+' — '+esc(n.label);
    sel.appendChild(opt);
  });
}

async function fetchKnnStatus(){
  try{
    const r=await fetch('/api/knn/status');if(!r.ok)return;
    const d=await r.json();
    const lb=$('knnLoadedBadge');if(lb)lb.style.display=d.loaded?'':'none';
    sT('knnProfileCount',d.loaded?d.profileCount:'--');
    sT('knnLabelCount',  d.loaded?d.labelCount:'--');
    sT('knnModelSrc',    d.loaded?(d.fromEmbed?'Firmware (embedded)':'FRAM'):'Not loaded');
    if(d.loaded)knnFetchLabels();
  }catch(e){}
}

async function knnFetchLabels(){
  try{
    const r=await fetch('/api/knn/labels');if(!r.ok)return;
    const d=await r.json();
    const dl=$('knnLabelOpts');if(!dl)return;
    dl.innerHTML=(d.labels||[]).filter(Boolean).map(l=>`<option value="${esc(l)}">`).join('');
  }catch(e){}
}

function isKnnMgmt(){return localStorage.getItem('knnMgmt')==='1';}
function setKnnMgmt(on){
  localStorage.setItem('knnMgmt',on?'1':'0');
  applyKnnMgmt();
}
function applyKnnMgmt(){
  const on=isKnnMgmt();
  const toggle=$('knnMgmtToggle');if(toggle)toggle.checked=on;
  const ctrl=$('knnMgmtControls');if(ctrl)ctrl.style.display=on?'':'none';
  const dtp=$('detailTrainPanel');
  if(dtp)dtp.style.display=(gwHasKnn&&on)?'':'none';
}

async function knnTrainStart(){
  const nodeId=cNid;
  const label=($('knnTrainLabel')&&$('knnTrainLabel').value.trim())||'';
  const dur=parseInt(($('knnTrainDur')&&$('knnTrainDur').value)||'5');
  const st=$('knnTrainStatus');
  if(!nodeId){if(st){st.style.color='var(--wn)';st.textContent='No node selected';}return;}
  if(!label){if(st){st.style.color='var(--wn)';st.textContent='Enter a label';}return;}
  try{
    const fd=new URLSearchParams({nodeId,label,durationMin:dur});
    const r=await fetch('/api/knn/train/start',{method:'POST',body:fd,headers:{'Content-Type':'application/x-www-form-urlencoded'}});
    const d=await r.json();
    if(d.error){if(st){st.style.color='var(--wn)';st.textContent=d.error;}return;}
    if(st)st.textContent='';
    knnStartPoll();
  }catch(e){if(st){st.style.color='var(--wn)';st.textContent='Request failed';}}
}

async function knnTrainStop(nodeId){
  try{
    const fd=new URLSearchParams({nodeId});
    await fetch('/api/knn/train/stop',{method:'POST',body:fd,headers:{'Content-Type':'application/x-www-form-urlencoded'}});
    await knnFetchTrainStatus();fetchKnnStatus();
  }catch(e){}
}

async function knnFetchTrainStatus(){
  try{
    const r=await fetch('/api/knn/train/status');if(!r.ok)return;
    const d=await r.json();
    knnRenderSessions(d.sessions||[]);
    if(!(d.sessions||[]).some(s=>s.active)){clearInterval(knnPollIv);knnPollIv=null;fetchKnnStatus();}
  }catch(e){}
}

function knnStartPoll(){
  if(knnPollIv)return;
  knnFetchTrainStatus();
  knnPollIv=setInterval(knnFetchTrainStatus,3000);
}

function updateKnnTrainBadge(){
  const training=gwHasKnn&&knnTrainingNodes.has(cNid);
  const dot='<svg class="rec-dot" width="7" height="7" viewBox="0 0 24 24" fill="#f87171" stroke="none" aria-hidden="true"><circle cx="12" cy="12" r="10"/></svg>';
  const tb=$('detTrainBadge');
  if(tb){tb.style.display=training?'':'none';if(training)tb.innerHTML=dot+' Recording';}
  const pb=$('trainPanelBadge');
  if(pb){pb.style.display=training?'':'none';if(training)pb.innerHTML=dot+' Recording';}
  const startBtn=$('knnTrainBtn');
  const stopBtn=$('knnStopBtn');
  if(startBtn)startBtn.style.display=training?'none':'';
  if(stopBtn)stopBtn.style.display=training?'':'none';
  const lbl=$('knnTrainLabel');const dur=$('knnTrainDur');
  if(lbl)lbl.disabled=training;if(dur)dur.disabled=training;
}

function knnFmtCountdown(sec){
  const s=Math.max(0,Math.round(sec));
  return Math.floor(s/60)+':'+(s%60<10?'0':'')+s%60;
}

function knnTickCountdown(){
  const rem=(knnCountdownEndMs-Date.now())/1000;
  const el=$('knnCountdownVal');
  if(el)el.textContent=knnFmtCountdown(rem);
  if(rem<=0){
    clearInterval(knnCountdownIv);knnCountdownIv=null;
    const cd=$('knnTrainCountdown');if(cd)cd.style.display='none';
  }
}

function knnRenderSessions(sessions){
  knnTrainingNodes=new Set((sessions||[]).filter(s=>s.active).map(s=>s.slotIdx+1));
  updateKnnTrainBadge();
  renderGrid(NC);
  // update countdown for the currently viewed node
  const activeSess=(sessions||[]).find(s=>s.active&&(s.slotIdx+1)===cNid);
  const cd=$('knnTrainCountdown');
  if(activeSess&&cd){
    knnCountdownEndMs=Date.now()+(activeSess.remainSec||0)*1000;
    cd.style.display='';
    if(!knnCountdownIv){knnCountdownIv=setInterval(knnTickCountdown,1000);}
    knnTickCountdown();
  }else if(cd&&cd.style.display!=='none'){
    cd.style.display='none';
    clearInterval(knnCountdownIv);knnCountdownIv=null;
  }
  const sp=$('knnSessionsPanel');
  const el=$('knnSessionList');
  if(!el)return;
  if(!sessions.length){
    if(sp)sp.style.display='none';
    el.innerHTML='';
    return;
  }
  if(sp)sp.style.display='';
  el.innerHTML=sessions.map(s=>{
    const rs=s.remainSec||0;
    const remTxt=s.active?(rs>60?Math.floor(rs/60)+'m '+Math.floor(rs%60)+'s':rs+'s'):(s.finalized?'Saved':'--');
    const stColor=s.finalized?'var(--ac)':s.active?'#f87171':'var(--txd)';
    const stLabel=s.finalized?'Saved':s.active?'Recording':'Stopped';
    const nodeLabel=NC.find(n=>n.id===(s.slotIdx+1));
    const nodeTxt=nodeLabel?(nodeLabel.label||('Node '+(s.slotIdx+1))):'Node '+(s.slotIdx+1);
    return `<div style="display:flex;align-items:center;gap:8px;padding:6px 0;border-bottom:1px solid var(--bd);font-size:11px">
      <span class="M" style="min-width:56px;font-size:9px;color:var(--txd);white-space:nowrap">${esc(nodeTxt)}</span>
      <span class="M" style="flex:1;font-weight:600;overflow:hidden;text-overflow:ellipsis;white-space:nowrap">${esc(s.label)}</span>
      <span class="M" style="font-size:9px;color:var(--txd);white-space:nowrap">${s.samples||0} smp</span>
      <span class="M" style="font-size:9px;color:${stColor};white-space:nowrap">${stLabel}</span>
      <span class="M" style="font-size:9px;color:var(--txd);min-width:34px;text-align:right">${remTxt}</span>
    </div>`;
  }).join('');
}

async function knnExport(){
  try{
    const r=await fetch('/api/knn/export');if(!r.ok){showToast('Export failed','error');return;}
    const blob=await r.blob();const url=URL.createObjectURL(blob);
    const a=document.createElement('a');a.href=url;a.download='knn_model.json';a.click();
    URL.revokeObjectURL(url);
  }catch(e){showToast('Export failed','error');}
}

async function knnImport(input){
  const file=input.files[0];if(!file)return;
  const st=$('knnModelStatus');
  if(st){st.style.color='var(--txd)';st.textContent='Importing...';}
  try{
    const text=await file.text();
    const r=await fetch('/api/knn/import',{method:'POST',body:text,headers:{'Content-Type':'application/json'}});
    const d=await r.json();
    if(d.error){if(st){st.style.color='var(--wn)';st.textContent=d.error;}}
    else{if(st){st.style.color='var(--ac)';st.textContent='Imported ('+d.profileCount+' profiles)';} fetchKnnStatus();}
  }catch(e){if(st){st.style.color='var(--wn)';st.textContent='Import failed';}}
  input.value='';
}

function promptKnnReset(a){
  openConfirm('Reset Inference Model?','Erase all trained appliance profiles from FRAM. This cannot be undone.',async()=>{
    try{
      const r=await fetch('/api/knn/profiles',{method:'DELETE'});
      const d=await r.json();
      if(d.ok){fetchKnnStatus();showToast('Model reset','ok');}
    }catch(e){showToast('Reset failed','error');}
  },'Reset',true,a);
}

/* -- Provision --------------------------------------------------- */
let provPollIv=null;

async function doProvision(){
  const btn=$('provBtn'),st=$('provStatus');
  if(!btn||btn.disabled)return;
  btn.disabled=true;
  if(st){st.style.color='var(--txd)';st.textContent='Waiting for card...';}
  try{
    const r=await fetch('/api/provision',{method:'POST'});
    if(r.status===409){
      if(st){st.style.color='var(--wn)';st.textContent='Already in progress';}
      btn.disabled=false;return;
    }
  }catch(e){
    if(st){st.style.color='var(--dg)';st.textContent='Request failed';}
    btn.disabled=false;return;
  }
  clearInterval(provPollIv);
  provPollIv=setInterval(async()=>{
    try{
      const r=await fetch('/api/provision/status');
      const d=await r.json();
      if(d.status==='ok'){
        clearInterval(provPollIv);
        if(st){st.style.color='var(--ac)';st.textContent='Key written';}
        setTimeout(()=>{if(st)st.textContent='';btn.disabled=false;},3000);
      }else if(d.status==='fail'){
        clearInterval(provPollIv);
        if(st){st.style.color='var(--dg)';st.textContent='Write failed -- try again';}
        btn.disabled=false;
      }
    }catch(e){clearInterval(provPollIv);btn.disabled=false;}
  },500);
}

/* -- AP security ------------------------------------------------- */
async function apLoadInfo(){
  try{
    const r=await fetch('/api/ap');
    if(!r.ok)return;
    const d=await r.json();
    const name=$('apSsidName');if(name)name.textContent=d.ssid;
    const status=$('apSsidStatus');if(status)status.textContent=d.hasPassword?'· Protected':'· Open';
    const inp=$('apPwInput');if(inp)inp.placeholder=d.hasPassword?'Enter password to change':'Leave blank for open network';
  }catch(_){}
}

function apTogglePw(){
  const inp=$('apPwInput');
  const ico=$('apEyeIco');
  if(!inp)return;
  const show=inp.type==='password';
  inp.type=show?'text':'password';
  if(ico)ico.innerHTML=show
    ?'<path d="M17.94 17.94A10.07 10.07 0 0 1 12 20c-7 0-11-8-11-8a18.45 18.45 0 0 1 5.06-5.94M9.9 4.24A9.12 9.12 0 0 1 12 4c7 0 11 8 11 8a18.5 18.5 0 0 1-2.16 3.19m-6.72-1.07a3 3 0 1 1-4.24-4.24"/><line x1="1" y1="1" x2="23" y2="23"/>'
    :'<path d="M1 12s4-8 11-8 11 8 11 8-4 8-11 8-11-8-11-8z"/><circle cx="12" cy="12" r="3"/>';
}

const AP_PW_HINT='Minimum 8 characters · leave blank for open network';
function apSetHint(msg,color){
  const h=$('apPwHint');if(!h)return;
  h.style.color=color||'var(--txd)';
  h.textContent=msg;
}

function apSavePassword(a){
  const inp=$('apPwInput');if(!inp)return;
  const pw=inp.value.trim();
  if(pw.length>0&&pw.length<8){apSetHint('Minimum 8 characters required','var(--err,#e05252)');return;}
  openConfirm(
    'Set AP Password',
    pw?'Set new AP WiFi password?':'Remove AP password (open network)?',
    execApSavePassword,'Save',false,a
  );
}

async function execApSavePassword(){
  const inp=$('apPwInput');if(!inp)return;
  const pw=inp.value.trim();
  apSetHint('Saving...','var(--txd)');
  try{
    const fd=new FormData();fd.append('password',pw);
    const r=await fetch('/api/ap',{method:'POST',body:new URLSearchParams(fd)});
    const d=await r.json();
    if(d.ok){
      inp.value='';
      apSetHint(pw?'Password set':'Password cleared','var(--ac)');
      await apLoadInfo();
      setTimeout(()=>apSetHint(AP_PW_HINT),3000);
    }else{
      apSetHint(d.message||'Error','var(--err,#e05252)');
    }
  }catch(_){
    apSetHint('Request failed','var(--err,#e05252)');
  }
}

/* -- Auth overlay ------------------------------------------------ */
function showLoginOverlay(){
  const el=$('authOverlay');if(el)el.style.display='flex';
  window._authFetch('/api/info').then(r=>r.json()).then(d=>{
    const sub=$('authApSsid');if(sub&&d.apSsid)sub.textContent=d.apSsid;
  }).catch(()=>{});
}

function hideLoginOverlay(){
  const el=$('authOverlay');if(el)el.style.display='none';
}

function loginTogglePw(){
  const inp=$('loginPwInput');
  const ico=$('loginEyeIco');
  if(!inp)return;
  const show=inp.type==='password';
  inp.type=show?'text':'password';
  if(ico)ico.innerHTML=show
    ?'<path d="M17.94 17.94A10.07 10.07 0 0 1 12 20c-7 0-11-8-11-8a18.45 18.45 0 0 1 5.06-5.94M9.9 4.24A9.12 9.12 0 0 1 12 4c7 0 11 8 11 8a18.5 18.5 0 0 1-2.16 3.19m-6.72-1.07a3 3 0 1 1-4.24-4.24"/><line x1="1" y1="1" x2="23" y2="23"/>'
    :'<path d="M1 12s4-8 11-8 11 8 11 8-4 8-11 8-11-8-11-8z"/><circle cx="12" cy="12" r="3"/>';
}

async function doLogin(){
  const inp=$('loginPwInput');
  const hint=$('loginHint');
  if(!inp||!hint)return;
  hint.style.color='var(--txd)';
  hint.textContent='Verifying...';
  try{
    const fd=new FormData();fd.append('password',inp.value);
    const r=await window._authFetch('/api/login',{method:'POST',body:new URLSearchParams(fd)});
    const d=await r.json();
    if(d.ok){
      authToken=d.token;
      localStorage.setItem('pt-token',authToken);
      inp.value='';hint.textContent='';
      hideLoginOverlay();startApp();
    }else{
      hint.style.color='var(--err,#e05252)';
      hint.textContent='Incorrect password';
      inp.value='';inp.focus();
    }
  }catch(_){
    hint.style.color='var(--err,#e05252)';
    hint.textContent='Connection failed';
  }
}

async function doLogout(){
  await fetch('/api/logout').catch(()=>{});
  authToken=null;
  localStorage.removeItem('pt-token');
  if(socket){socket.close();socket=null;wsOk=false;}
  showLoginOverlay();
}

/* -- Dashboard security ------------------------------------------ */
const DASH_PW_HINT='Minimum 8 characters · leave blank for open access';

function dashTogglePw(){
  const inp=$('dashPwInput');
  const ico=$('dashEyeIco');
  if(!inp)return;
  const show=inp.type==='password';
  inp.type=show?'text':'password';
  if(ico)ico.innerHTML=show
    ?'<path d="M17.94 17.94A10.07 10.07 0 0 1 12 20c-7 0-11-8-11-8a18.45 18.45 0 0 1 5.06-5.94M9.9 4.24A9.12 9.12 0 0 1 12 4c7 0 11 8 11 8a18.5 18.5 0 0 1-2.16 3.19m-6.72-1.07a3 3 0 1 1-4.24-4.24"/><line x1="1" y1="1" x2="23" y2="23"/>'
    :'<path d="M1 12s4-8 11-8 11 8 11 8-4 8-11 8-11-8-11-8z"/><circle cx="12" cy="12" r="3"/>';
}

function dashSetHint(msg,color){
  const h=$('dashPwHint');if(!h)return;
  h.style.color=color||'var(--txd)';
  h.textContent=msg;
}

async function dashLoadSecure(){
  try{
    const r=await fetch('/api/dashsecure');if(!r.ok)return;
    const d=await r.json();
    const btn=$('dashLogoutBtn');if(btn)btn.style.display=d.hasPassword?'':'none';
    const inp=$('dashPwInput');if(inp)inp.placeholder=d.hasPassword?'Password set -- enter new to change':'Leave blank for open access';
  }catch(_){}
}

function dashSavePassword(a){
  const inp=$('dashPwInput');if(!inp)return;
  const pw=inp.value.trim();
  if(pw.length>0&&pw.length<8){dashSetHint('Minimum 8 characters required','var(--err,#e05252)');return;}
  openConfirm(
    'Set Dashboard Password',
    pw?'Set new dashboard login password?':'Remove dashboard password (open access)?',
    execDashSavePassword,'Save',false,a
  );
}

async function execDashSavePassword(){
  const inp=$('dashPwInput');if(!inp)return;
  const pw=inp.value.trim();
  dashSetHint('Saving...','var(--txd)');
  try{
    const fd=new FormData();fd.append('password',pw);
    const r=await fetch('/api/dashsecure',{method:'POST',body:new URLSearchParams(fd)});
    const d=await r.json();
    if(d.ok){
      inp.value='';
      dashSetHint(pw?'Password set -- you will be logged out':'Password cleared','var(--ac)');
      await dashLoadSecure();
      if(pw){setTimeout(()=>doLogout(),2000);}
      else{setTimeout(()=>dashSetHint(DASH_PW_HINT),3000);}
    }else{
      dashSetHint(d.message||'Error','var(--err,#e05252)');
    }
  }catch(_){
    dashSetHint('Request failed','var(--err,#e05252)');
  }
}

/* -- Reboot ------------------------------------------------------ */
async function execReboot(){
  try{await fetch('/api/reboot',{method:'POST'});}catch(e){}
  sT('gwUptime','Rebooting...');
  setTimeout(()=>{
    const poll=setInterval(async()=>{
      try{await fetch('/api/features');clearInterval(poll);location.reload();}catch(e){}
    },1500);
  },2000);
}

/* -- Init -------------------------------------------------------- */
function startApp(){
  connectWS();
  route();
  setInterval(fetchSys,10000);
  fetchSys();
  fetchFeatures();
  apLoadInfo();
  dashLoadSecure();
  wifiLoadStaticIp();
}

function init(){
  document.addEventListener('DOMContentLoaded',async()=>{
    applyTheme(getTheme());
    applyKnnMgmt();
    const cri=$('costRateIn');if(cri)cri.value=costRate;
    const bcl=$('btnClearLog');if(bcl)bcl.addEventListener('click',()=>{const b=$('serialLogBox');if(b)b.innerHTML='';});
    buildLogFilterRow();
    setSettingsTab(localStorage.getItem('pt-stab')||'network');
    try{
      const r=await window._authFetch('/api/authstatus');
      const d=await r.json();
      if(!d.hasPassword){
        authToken=null;localStorage.removeItem('pt-token');
        hideLoginOverlay();startApp();
      }else if(authToken){
        const probe=await fetch('/api/status');
        if(probe.ok){hideLoginOverlay();startApp();}
      }else{
        showLoginOverlay();
      }
    }catch(_){
      // No gateway reachable (dev/sim server). Bypass auth so simulation.js can run.
      hideLoginOverlay();
      startApp();
    }
  });
}
