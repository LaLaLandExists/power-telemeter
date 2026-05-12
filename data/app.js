/* -- State ------------------------------------------------------- */
let cNid=null,chart=null,cMet='power',socket=null,wsOk=false,fbT=null;
let NC=[],gwTimeSet=false,gwTime='--:--:--';
let pendingCmd={};
let relayOffAt={};
let currentRules={};    // nodeId → AutoRule[] from /api/node/{id}/rules
let builderScheds=[];   // schedule rows in the rule builder
let builderProts=[];    // protection rows in the rule builder
let costRate=parseFloat(localStorage.getItem('pt-costRate'))||12.00;

const MC={
  power:  {l:'Power (W)',  c:'#118ab2', k:'p', n:'power'},
  voltage:{l:'Voltage (V)',c:'#ffd166', k:'v', n:'voltage'},
  current:{l:'Current (A)',c:'#06d6a0', k:'i', n:'current'}
};
const LOG_MAX_LINES=200;
const LOG_PREFIX_CLASS={
  'GW-UL':'ok','GW-BCN':'ok','GW-DL':'ok','GW-CW':'ok','GW-TDMA':'ok',
  'WIFI':'info','WEB':'info','WS':'info','FRAM':'info','FS':'info','LORA':'info',
};
const logSuppressed=new Set();
const SL=['','Waiting','Active'];
const SC=['','sched-badge waiting','sched-badge active'];
const NUDGE_ICO='<svg width="10" height="10" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2"><circle cx="12" cy="12" r="10"/><line x1="12" y1="8" x2="12" y2="12"/><line x1="12" y1="16" x2="12.01" y2="16"/></svg>';
const NUDGED_ICO='<svg width="10" height="10" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round"><polyline points="20 6 9 17 4 12"/></svg>';
const RELAY_WARN_W=5;       // watts: relay-off but still drawing power triggers warning
const SUPERFRAME_MS=3000;   // grace period before showing relay-ineffective warning

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
function esc(s){const d=document.createElement('div');d.textContent=s;return d.innerHTML;}
function pad2(n){return String(n).padStart(2,'0');}
function rssiToBars(r){if(r>=-55)return 4;if(r>=-65)return 3;if(r>=-75)return 2;return 1;}

/* -- Clock ------------------------------------------------------- */
function updateClock(){const d=new Date();$('sysClock').textContent=pad2(d.getHours())+':'+pad2(d.getMinutes());}
setInterval(updateClock,10000);updateClock();

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
  socket.onopen=()=>{wsOk=true;wsDot(true);syncTime();wsSend({cmd:'get_nodes'});};
  socket.onmessage=e=>{try{onMsg(JSON.parse(e.data));}catch(x){}};
  socket.onclose=()=>{wsOk=false;wsDot(false);startFallback();setTimeout(connectWS,3000);};
  socket.onerror=()=>socket.close();
}
function wsSend(o){if(wsOk&&socket&&socket.readyState===1)socket.send(JSON.stringify(o));}
function wsDot(ok){
  const el=$('wsStatus');
  el.className='hdr-badge M '+(ok?'ws-ok':'ws-err');
  el.innerHTML=`<svg width="8" height="8" viewBox="0 0 10 10" style="display:inline;margin-right:4px"><circle cx="5" cy="5" r="4" fill="currentColor"/></svg>${ok?'WS':'WS✘'}`;
}
function syncTime(){const d=new Date();wsSend({cmd:'set_time',hour:d.getHours(),minute:d.getMinutes(),second:d.getSeconds()});}
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
}

/* -- Message handler --------------------------------------------- */
function onMsg(m){
  if(m.timeSet!==undefined)gwTimeSet=m.timeSet;
  if(m.time)gwTime=m.time;
  updGwTime();

  if(m.type==='nodes'){
    NC=m.nodes||[];
    NC.forEach(n=>{
      if((n.relayState||0)===0){if(relayOffAt[n.id]===undefined)relayOffAt[n.id]=Date.now();}
      else delete relayOffAt[n.id];
    });
    if(!cNid)renderGrid(NC);
    sT('idxCount',m.count||0);
    $('nodeCount').textContent=(m.count||0)+' node'+((m.count||0)!==1?'s':'');
  }
  else if(m.type==='telemetry'){
    const n=m.node;
    const prev=NC.find(x=>x.id===n.id);
    const prevRs=prev?prev.relayState:undefined;
    if(n.relayState===1){delete relayOffAt[n.id];}
    else if(n.relayState===0&&prevRs!==0){relayOffAt[n.id]=Date.now();}
    const o={id:n.id,label:n.label,online:n.online,rssi:n.rssi,voltage:n.voltage,current:n.current,power:n.power,energy:n.energy,frequency:n.frequency,powerFactor:n.powerFactor,relayState:n.relayState,alarmState:n.alarmState,age:n.age,pending:n.pending,ruleStatus:n.ruleStatus};
    const i=NC.findIndex(x=>x.id===n.id);if(i>=0)Object.assign(NC[i],o);else NC.push(o);
    if(!cNid)renderGrid(NC);
    if(cNid===n.id){updateDetail(o);addChartPoint(n);}
  }
  else if(m.type==='name_changed'){
    const i=NC.findIndex(x=>x.id===m.node);
    if(i>=0)NC[i].label=m.name;
    if(cNid===m.node&&!document.querySelector('.rename-input'))sT('detLabel',m.name);
    if(!cNid)renderGrid(NC);
  }
  else if(m.type==='time_set'){gwTimeSet=true;if(m.time)gwTime=m.time;updGwTime();}
  else if(m.type==='threshold_ack'){if(!m.success){if(m.node!==undefined)delete pendingCmd[m.node];alert('Threshold command failed');}}
  else if(m.type==='nudge_ack'){if(!m.success){if(m.node!==undefined)delete pendingCmd[m.node];alert('Nudge failed');}}
  else if(m.type==='relay_ack'||m.type==='auto_ack'){if(!m.success){if(m.node!==undefined)delete pendingCmd[m.node];alert('Command failed');}}
  else if(m.type==='rules_queued'){if(!m.success)alert('Rules delivery failed — check node is online and try again.');}
  else if(m.type==='energy_cleared'){const i=NC.findIndex(x=>x.id===m.node);if(i>=0)NC[i].energy=0;if(cNid===m.node)updateDetail(NC.find(x=>x.id===cNid)||{});}
  else if(m.type==='all_energy_cleared'){NC.forEach(n=>n.energy=0);if(cNid){const c=NC.find(x=>x.id===cNid);if(c)updateDetail(c);}if(!cNid)renderGrid(NC);}
  else if(m.type==='log')appendLog(m.line);
}

/* -- Fallback polling -------------------------------------------- */
function startFallback(){
  if(fbT)return;
  fbT=setInterval(async()=>{
    try{
      if(cNid){const r=await fetch('/api/node/'+cNid+'/live');const d=await r.json();if(!d.error)updateDetail(d);}
      else{const r=await fetch('/api/nodes');const d=await r.json();NC=d.nodes||[];renderGrid(NC);}
    }catch(e){}
  },3000);
}

/* -- Routing ----------------------------------------------------- */
function route(){const h=location.hash||'#/';const m=h.match(/^#\/node\/(\d+)$/);m?showDetail(parseInt(m[1])):showIndex();}
window.addEventListener('hashchange',route);
function goHome(){location.hash='#/';}
function goNode(id){location.hash='#/node/'+id;}

function showIndex(){
  cNid=null;
  document.body.classList.remove('vDet-active');
  $('vIdx').classList.add('active');$('vDet').classList.remove('active');
  $('backBtn').style.display='none';
  renderGrid(NC);wsSend({cmd:'get_nodes'});
}

/* -- Render node grid -------------------------------------------- */
function renderGrid(ns){
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
    if(isNew){c=document.createElement('div');c.className='node-card';c.dataset.id=n.id;c.onclick=()=>goNode(n.id);}
    const rs=n.relayState!==undefined?n.relayState:n.relay;
    const al=n.alarmState||n.alarm||0;
    const rw=n.online&&rs===0&&(n.power||0)>RELAY_WARN_W&&(Date.now()-(relayOffAt[n.id]||Date.now()))>SUPERFRAME_MS;
    const faved=isFav(n.id);
    const fp=`${n.label}|${n.online}|${(n.voltage||0).toFixed(1)}|${(n.current||0).toFixed(2)}|${fP(n.power||0)}|${rs}|${al}|${rw}|${faved}|${n.rssi}`;
    if(c._fp!==fp){
      c._fp=fp;
      c.classList.toggle('alarm',!!al);
      c.classList.toggle('relay-warn',rw&&!al);
      c.classList.toggle('offline',!n.online);
      const bars=rssiToBars(n.rssi||0);
      c.innerHTML=`
        <button class="fav-star ${faved?'faved':''}" onclick="toggleFav(${n.id},event)">${faved?'★':'☆'}</button>
        <div class="nc-head">
          <div>
            <div class="nc-id M">Node #${n.id}</div>
            <div class="nc-label">${esc(n.label)}</div>
          </div>
          <span class="nc-status ${n.online?'on':'off'} M">${n.online?'ONLINE':'OFFLINE'}</span>
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
            <div class="sig-bars">
              <div class="bar ${bars>=1?'lit':''}"></div>
              <div class="bar ${bars>=2?'lit':''}"></div>
              <div class="bar ${bars>=3?'lit':''}"></div>
              <div class="bar ${bars>=4?'lit':''}"></div>
            </div>
            ${n.online?(n.rssi||'--')+' dBm':'--'}
          </span>
          <button class="nudge-btn M" data-nid="${n.id}" onclick="doNudge(${n.id},event)" ${n.online?'':'disabled style="opacity:.3;pointer-events:none"'}>
            <svg width="10" height="10" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2"><circle cx="12" cy="12" r="10"/><line x1="12" y1="8" x2="12" y2="12"/><line x1="12" y1="16" x2="12.01" y2="16"/></svg>
            Nudge
          </button>
          <span class="nc-relay ${rs?'on':'off'}${rw?' warn':''}">Relay: ${rs?'ON':'OFF'}${rw?' ⚠':''}</span>
          <span class="nc-arrow">
            <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round"><polyline points="9 18 15 12 9 6"/></svg>
          </span>
        </div>
      `;
    }
    if(isNew||orderChanged)g.appendChild(c);
  });
}

/* -- Show detail ------------------------------------------------- */
function showDetail(id){
  cNid=id;
  document.body.classList.add('vDet-active');
  $('vIdx').classList.remove('active');
  $('vDet').classList.add('active');
  $('backBtn').style.display='flex';
  cMet='power';
  initChart();
  // Reset chart tab buttons via the single authoritative path
  document.querySelectorAll('[data-m]').forEach(b=>{
    const active=b.dataset.m==='power';
    b.style.borderColor=active?'var(--ac)':'';
    b.style.color=active?'var(--ac)':'';
    b.style.background=active?'rgba(0,229,160,.08)':'';
  });
  fetchHistory(id);fetchSys();loadNodeRules(id);
  renderLoadoutSelector();updateLoadoutStatus();
  const c=NC.find(n=>n.id===id);if(c)updateDetail(c);
}

/* -- Update detail view ------------------------------------------ */
function updateDetail(d){
  if(!document.querySelector('.rename-input'))sT('detLabel',d.label||('Node '+d.id));
  $('detSub').textContent=`ID: ${d.id}  •  LoRa: ${d.rssi||'--'} dBm  •  ${d.age||0}s ago`;

  const st=$('detStatus');
  st.textContent=d.online?'ONLINE':'OFFLINE';
  st.className='panel-badge M '+(d.online?'ac':'dg');

  sT('dV',(d.voltage||0).toFixed(1));
  sT('dI',(d.current||0).toFixed(3));
  sT('dP',(d.power||0).toFixed(1));
  if((d.energy||0)>=10000){sT('dE',((d.energy||0)/1000).toFixed(2));sT('dEu','kWh');}
  else{sT('dE',String(d.energy||0));sT('dEu','Wh');}
  sT('dF',(d.frequency||0).toFixed(1));
  sT('dPF',(d.powerFactor||0).toFixed(2));

  sB('bV',d.voltage||0,260);sB('bI',d.current||0,100);sB('bP',d.power||0,23000);
  sB('bE',d.energy||0,50000);sB('bF',d.frequency||0,65);sB('bPF',d.powerFactor||0,1);

  const al=d.alarmState||d.alarm||0;
  $('pwrCard').classList.toggle('alarm-card',!!al);
  $('alarmTag').classList.toggle('show',!!al);

  const rs=d.relayState!==undefined?d.relayState:(d.relay||0);
  const on=rs===1;
  const rw=d.online&&rs===0&&(d.power||0)>RELAY_WARN_W&&(Date.now()-(relayOffAt[d.id]||Date.now()))>SUPERFRAME_MS;
  $('relayState').textContent=on?'ON':'OFF';
  $('relayState').className='relay-state-val '+(on?'on':'off')+' M';
  $('relayToggle').checked=on;
  $('relayWarnTag').classList.toggle('show',rw);
  $('relayStateBox').classList.toggle('warn',rw);
  const ruleStatus=d.ruleStatus||{};
  const autoActive=!!(ruleStatus.engineActive);
  const hasRules=!!(ruleStatus.hasRules||ruleStatus.count>0);
  $('relayMode').textContent=autoActive?'AUTO':'MANUAL';

  if(!d.pending&&pendingCmd[d.id]!==undefined)delete pendingCmd[d.id];
  const pend=d.pending||false;
  const cmdType=pendingCmd[d.id]||null;
  $('commitBanner').classList.toggle('show',pend&&d.online!==false);
  $('relayTabs').classList.toggle('frozen',pend);
  $('relayInner').classList.toggle('frozen',pend);

  const off=!d.online;
  $('offlineMsg').classList.toggle('show',off);
  $('relayCtrl').style.display=off?'none':'block';
  $('thrCtrl').classList.toggle('frozen',off||pend);
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

  // Lock manual toggle when auto mode is active
  const relayToggle=$('relayToggle');
  if(relayToggle){
    relayToggle.disabled=autoActive||off||pend;
    relayToggle.style.opacity=(autoActive||off||pend)?'.4':'';
  }
  const manualHint=$('manualAutoHint');
  if(manualHint)manualHint.style.display=autoActive?'block':'none';

  // Update auto tab button
  const autoEnBtn=$('autoEnableBtn'),autoDsBtn=$('autoDisableBtn'),autoNoRules=$('autoNoRulesHint');
  if(autoEnBtn)autoEnBtn.style.display=(!autoActive&&hasRules&&!off&&!pend)?'block':'none';
  if(autoDsBtn)autoDsBtn.style.display=(autoActive&&!off&&!pend)?'block':'none';
  if(autoNoRules)autoNoRules.style.display=(!hasRules&&!off)?'block':'none';

  // Update rule engine status UI in Automation Rules panel
  const reBadge=$('ruleEngineBadge');
  if(reBadge){
    const cnt=ruleStatus.count||0;
    reBadge.textContent=cnt+' rule'+(cnt!==1?'s':'');
    reBadge.className='panel-badge M '+(autoActive?'ac':'');
  }
  const latchBadge=$('ruleLatchedBadge');
  if(latchBadge)latchBadge.style.display=ruleStatus.protectionLatched?'':'none';
  const srcBadge=$('ruleSourceBadge');
  if(srcBadge){
    const srcCls={manual:'',protection:'dg',schedule:'ac',default:''};
    srcBadge.textContent=(ruleStatus.relaySource||'manual').toUpperCase();
    srcBadge.className='panel-badge M '+(srcCls[ruleStatus.relaySource||'manual']||'');
  }

  const cost=((d.energy||0)/1000)*costRate;
  $('costVal').textContent=cost.toFixed(2);
  $('costRate2').textContent='@ '+costRate.toFixed(2)+' / kWh';
  $('costEnergy').textContent=(d.energy||0)+' Wh';


}

/* -- Relay controls ---------------------------------------------- */
function setRelayTab(m){
  document.querySelectorAll('#relayTabs div').forEach(t=>t.classList.toggle('active',t.dataset.mode===m));
  $('tabManual').classList.toggle('active',m==='manual');
  $('tabAuto').classList.toggle('active',m==='auto');
}
function freezeAllCmds(){
  $('commitBanner').classList.add('show');
  $('relayTabs').classList.add('frozen');
  $('relayInner').classList.add('frozen');
  $('thrCtrl').classList.add('frozen');
  const nb=$('detNudge');
  if(nb){nb.disabled=true;nb.style.opacity='.3';nb.style.pointerEvents='none';}
}
function doManual(c){pendingCmd[cNid]='relay_manual';wsSend({cmd:'relay_manual',node:cNid,state:c?1:0});freezeAllCmds();}
function doEnableAuto(){
  if(!cNid)return;
  pendingCmd[cNid]='auto';
  wsSend({cmd:'auto_enable',node:cNid});
  freezeAllCmds();
}
function doDisableAuto(){
  if(!cNid)return;
  pendingCmd[cNid]='auto';
  wsSend({cmd:'auto_disable',node:cNid});
  freezeAllCmds();
}
function doSetThreshold(){
  const v=parseInt($('thrInput').value);
  if(!v||v<1||v>23000){alert('Enter 1–23000 W');return;}
  pendingCmd[cNid]='threshold';
  wsSend({cmd:'set_threshold',node:cNid,watts:v});
  freezeAllCmds();
}

/* -- Confirm dialogs --------------------------------------------- */
function showConfirm(id){$(id).classList.add('show');}
function hideConfirm(id){$(id).classList.remove('show');}
function execClearEnergy(){hideConfirm('cfmEnergy');wsSend({cmd:'clear_energy',node:cNid});}
function execClearAllEnergy(){hideConfirm('cfmAllEnergy');wsSend({cmd:'clear_all_energy'});}

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
  if(!v||v<=0){alert('Enter a valid rate.');return;}
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
    h2.onclick=startRename;h2.title='Click to rename';
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
          titleFont:{family:"'JetBrains Mono'",size:10},bodyFont:{family:"'JetBrains Mono'",size:11},
          padding:8,cornerRadius:5
        }
      },
      scales:{
        x:{grid:{color:getCS('--chart-grid')},ticks:{color:getCS('--chart-tx'),font:{family:"'JetBrains Mono'",size:9},maxTicksLimit:8}},
        y:{grid:{color:getCS('--chart-grid')},ticks:{color:getCS('--chart-tx'),font:{family:"'JetBrains Mono'",size:9}}}
      }
    }
  });
}

function swChart(m){
  cMet=m;
  // Update tab button styles
  document.querySelectorAll('[data-m]').forEach(b=>{
    const active=b.dataset.m===m;
    b.style.borderColor=active?'var(--ac)':'';
    b.style.color=active?'var(--ac)':'';
    b.style.background=active?'rgba(0,229,160,.08)':'';
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
    updGwTime();
    if(d.version)sT('fwVer',d.version);
    updNetPanel(d);
  }catch(e){}
}

function updNetPanel(d){
  const apMode = d.wifiMode==='ap' || !d.wifiRSSI;
  sT('gwSSID', d.ssid||'--');
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
  sT('gwIP', d.ip||'--');
}


/* -- WiFi config drawer ------------------------------------------ */
let wifiSelectedSSID = '';
let wifiPollIv = null;
let wifiDrawerOpen = false;
let wifiCachedStaticIp = ''; // populated on drawer open; shown in AP-dropped hint
let wifiStaticSaved = false; // true when static IP fields match what's persisted on the server

function isValidIp(s) {
  const p = s.split('.');
  if (p.length !== 4) return false;
  return p.every(o => o !== '' && /^\d+$/.test(o) && +o >= 0 && +o <= 255);
}

function isValidSubnet(s) {
  if (!isValidIp(s)) return false;
  const n = s.split('.').map(Number).reduce((a, b) => (a << 8) | b, 0) >>> 0;
  const inv = (~n) >>> 0;
  return (inv & (inv + 1)) === 0;
}

function wifiSetFieldErr(id, hasErr, msg) {
  const inp  = $(id);
  const hint = $(id + 'Err');
  inp.classList.toggle('err', hasErr);
  if (hint) { hint.textContent = hasErr ? msg : ''; hint.style.display = hasErr ? '' : 'none'; }
}

function wifiValidateStaticFields() {
  wifiStaticSaved = false;
  const ip  = $('wifiStaticIp').value.trim();
  const gw  = $('wifiStaticGw').value.trim();
  const sn  = $('wifiStaticSn').value.trim();
  const dns = $('wifiStaticDns').value.trim();

  const ipOk  = isValidIp(ip);
  const gwOk  = isValidIp(gw);
  const snOk  = isValidSubnet(sn);
  const dnsOk = !dns || isValidIp(dns);

  wifiSetFieldErr('wifiStaticIp',  ip  && !ipOk,  'Invalid IP address');
  wifiSetFieldErr('wifiStaticGw',  gw  && !gwOk,  'Invalid gateway address');
  wifiSetFieldErr('wifiStaticSn',  sn  && !snOk,  'Invalid subnet mask');
  wifiSetFieldErr('wifiStaticDns', dns && !dnsOk, 'Invalid DNS address');

  // Subnet-mismatch warning (non-blocking)
  const gwWarn = $('wifiStaticGwWarn');
  if (ipOk && gwOk && snOk) {
    const ipP = ip.split('.').map(Number);
    const gwP = gw.split('.').map(Number);
    const snP = sn.split('.').map(Number);
    const same = ipP.every((b, i) => (b & snP[i]) === (gwP[i] & snP[i]));
    gwWarn.textContent = same ? '' : 'Gateway not on same subnet as IP';
    gwWarn.style.display = same ? 'none' : '';
  } else {
    gwWarn.textContent = ''; gwWarn.style.display = 'none';
  }

  const saveErr = $('wifiStaticSaveErr');
  saveErr.textContent = ''; saveErr.style.display = 'none';

  const allOk = ip && gw && sn && ipOk && gwOk && snOk && dnsOk;
  $('wifiStaticSaveBtn').disabled = !allOk;
  wifiValidateConnect();
}

function toggleWifiDrawer() {
  wifiDrawerOpen = !wifiDrawerOpen;
  const drawer = $('wifiDrawer');
  const btn    = $('wifiCfgBtn');
  drawer.classList.toggle('open', wifiDrawerOpen);
  btn.classList.toggle('ac-active', wifiDrawerOpen);
  if (wifiDrawerOpen) { wifiShowScan(); wifiLoadStaticIp(); }
}

/* -- Screen routing -------------------------------------------- */
function wifiShowScan() {
  $('wifiScr-scan').style.display       = '';
  $('wifiScr-connecting').style.display = 'none';
  $('wifiScr-result').style.display     = 'none';
}
function wifiShowConnecting(ssid) {
  $('wifiScr-scan').style.display       = 'none';
  $('wifiScr-connecting').style.display = '';
  $('wifiScr-result').style.display     = 'none';
  $('wifiConnTitle').textContent = 'Connecting…';
  $('wifiConnSub').textContent   = ssid;
}
function wifiShowResult(ok, ip, ssid) {
  $('wifiScr-scan').style.display       = 'none';
  $('wifiScr-connecting').style.display = 'none';
  $('wifiScr-result').style.display     = '';
  const ico = $('wifiResultIcon');
  if (ok) {
    ico.style.borderColor  = 'var(--ac)';
    ico.style.background   = 'rgba(0,229,160,.1)';
    ico.style.color        = 'var(--ac)';
    ico.innerHTML = '<svg width="22" height="22" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round"><polyline points="20 6 9 17 4 12"/></svg>';
  } else {
    ico.style.borderColor  = 'var(--dg)';
    ico.style.background   = 'rgba(255,56,96,.1)';
    ico.style.color        = 'var(--dg)';
    ico.innerHTML = '<svg width="22" height="22" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round"><line x1="18" y1="6" x2="6" y2="18"/><line x1="6" y1="6" x2="18" y2="18"/></svg>';
  }
  $('wifiResultTitle').textContent = ok ? 'Connected!' : 'Failed';
  $('wifiResultSub').innerHTML = ok
    ? `Gateway joined <strong>${esc(ssid)}</strong>.<br>IP: <span style="color:var(--ac)">${esc(ip)}</span>`
    : `Could not connect to <strong>${esc(ssid)}</strong>.<br>Check password and try again.`;
  const backBtn = $('wifiBackBtn');
  backBtn.onclick = ok ? (() => { toggleWifiDrawer() }) : (() => { wifiShowScan() });
}

/* -- Scan ------------------------------------------------------- */
function wifiDoScan() {
  const btn  = $('wifiScanBtn');
  const list = $('wifiNetList');
  btn.classList.add('spinning');
  btn.disabled = true;
  list.innerHTML = '<div style="text-align:center;padding:18px 0;color:var(--txd);font-size:11px;font-family:monospace">Scanning…</div>';
  let scanTries = 0;
  function poll() {
    fetch('/api/scan')
      .then(r => r.json())
      .then(d => {
        if (d.scanning && ++scanTries < 20) { setTimeout(poll, 800); return; }
        btn.classList.remove('spinning');
        btn.disabled = false;
        wifiRenderNetworks(d.networks || []);
      })
      .catch(() => {
        list.innerHTML = '<div style="text-align:center;padding:18px 0;color:var(--dg);font-size:11px;font-family:monospace">Scan failed — check connection</div>';
        btn.classList.remove('spinning');
        btn.disabled = false;
      });
  }
  poll();
}

function wifiRenderNetworks(nets) {
  const list = $('wifiNetList');
  if (!nets.length) {
    list.innerHTML = '<div style="text-align:center;padding:18px 0;color:var(--txd);font-size:11px;font-family:monospace">No networks found</div>';
    return;
  }
  const lockSvg = '<svg width="12" height="12" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><rect x="3" y="11" width="18" height="11" rx="2"/><path d="M7 11V7a5 5 0 0 1 10 0v4"/></svg>';
  const openSvg = '<svg width="12" height="12" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M1.42 9a16 16 0 0 1 21.16 0"/><path d="M5 12.55a11 11 0 0 1 14.08 0"/><path d="M8.53 16.11a6 6 0 0 1 6.95 0"/><line x1="12" y1="20" x2="12.01" y2="20" stroke-width="2.5"/></svg>';
  list.innerHTML = '';
  nets.sort((a, b) => b.rssi - a.rssi).forEach((n, i) => {
    const bars = rssiToBars(n.rssi);
    const el = document.createElement('div');
    el.className = 'wifi-net-item';
    el.style.animationDelay = (i * 0.04) + 's';
    el.style.animation = 'slideIn .3s ease both';
    el.innerHTML =
      '<span style="color:var(--txd);display:flex;align-items:center">' + (n.secure ? lockSvg : openSvg) + '</span>' +
      '<div class="wifi-net-info">' +
        '<div class="wifi-net-name">' + esc(n.ssid) + '</div>' +
        '<div class="wifi-net-meta">' + n.rssi + ' dBm · ' + (n.secure ? 'WPA2' : 'Open') + '</div>' +
      '</div>' +
      (bars>=4?'<svg width=\"15\" height=\"15\" viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"currentColor\" stroke-width=\"1.8\" stroke-linecap=\"round\" stroke-linejoin=\"round\"><path d=\"M1.42 9a16 16 0 0 1 21.16 0\"/><path d=\"M5 12.55a11 11 0 0 1 14.08 0\"/><path d=\"M8.53 16.11a6 6 0 0 1 6.95 0\"/><line x1=\"12\" y1=\"20\" x2=\"12.01\" y2=\"20\" stroke-width=\"2.5\"/></svg>':bars>=3?'<svg width=\"15\" height=\"15\" viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"currentColor\" stroke-width=\"1.8\" stroke-linecap=\"round\" stroke-linejoin=\"round\"><path d=\"M5 12.55a11 11 0 0 1 14.08 0\"/><path d=\"M8.53 16.11a6 6 0 0 1 6.95 0\"/><line x1=\"12\" y1=\"20\" x2=\"12.01\" y2=\"20\" stroke-width=\"2.5\"/></svg>':bars>=2?'<svg width=\"15\" height=\"15\" viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"currentColor\" stroke-width=\"1.8\" stroke-linecap=\"round\" stroke-linejoin=\"round\"><path d=\"M8.53 16.11a6 6 0 0 1 6.95 0\"/><line x1=\"12\" y1=\"20\" x2=\"12.01\" y2=\"20\" stroke-width=\"2.5\"/></svg>':'<svg width=\"15\" height=\"15\" viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"currentColor\" stroke-width=\"1.8\" stroke-linecap=\"round\" stroke-linejoin=\"round\"><line x1=\"12\" y1=\"20\" x2=\"12.01\" y2=\"20\" stroke-width=\"2.5\"/></svg>');
    el.addEventListener('click', () => wifiSelectNetwork(n.ssid, n.secure, el));
    list.appendChild(el);
  });
}

function wifiSelectNetwork(ssid, secure, el) {
  document.querySelectorAll('.wifi-net-item').forEach(i => i.classList.remove('selected'));
  el.classList.add('selected');
  wifiSelectedSSID = ssid;
  // Uncheck manual if was checked
  $('wifiManualCb').checked = false;
  wifiToggleManual();
  // Show cred section
  $('wifiCredSection').style.display = 'block';
  const pw = $('wifiPwInput');
  if (!secure) { pw.value = ''; pw.placeholder = 'No password required'; pw.disabled = true; }
  else         { pw.placeholder = 'Enter password…'; pw.disabled = false; pw.focus(); }
  wifiValidateConnect();
}

/* -- Manual SSID toggle ----------------------------------------- */
function wifiToggleManual() {
  const manual = $('wifiManualCb').checked;
  $('wifiManualInput').style.display = manual ? 'block' : 'none';
  if (manual) {
    document.querySelectorAll('.wifi-net-item').forEach(i => i.classList.remove('selected'));
    wifiSelectedSSID = '';
    $('wifiCredSection').style.display = 'block';
    const pw = $('wifiPwInput');
    pw.placeholder = 'Enter password…'; pw.disabled = false;
    $('wifiManualSSID').focus();
  } else {
    if (!wifiSelectedSSID) $('wifiCredSection').style.display = 'none';
  }
  wifiValidateConnect();
}

function wifiValidateConnect() {
  const manual = $('wifiManualCb').checked;
  const ssid   = manual ? $('wifiManualSSID').value.trim() : wifiSelectedSSID;

  let staticOk = true;
  if ($('wifiStaticIpCb').checked) {
    const ip = $('wifiStaticIp').value.trim();
    const gw = $('wifiStaticGw').value.trim();
    const sn = $('wifiStaticSn').value.trim();
    staticOk = wifiStaticSaved && isValidIp(ip) && isValidIp(gw) && isValidSubnet(sn);
  }

  const btn = $('wifiConnectBtn');
  btn.disabled = !ssid || !staticOk;
  btn.title = !staticOk ? 'Save valid static IP settings first' : '';
}

/* -- Password eye ----------------------------------------------- */
function wifiTogglePw() {
  const inp  = $('wifiPwInput');
  const ico  = $('wifiEyeIco');
  const show = inp.type === 'password';
  inp.type = show ? 'text' : 'password';
  ico.innerHTML = show
    ? '<path d="M17.94 17.94A10.07 10.07 0 0 1 12 20c-7 0-11-8-11-8a18.45 18.45 0 0 1 5.06-5.94"/><path d="M9.9 4.24A9.12 9.12 0 0 1 12 4c7 0 11 8 11 8a18.5 18.5 0 0 1-2.16 3.19"/><line x1="1" y1="1" x2="23" y2="23"/>'
    : '<path d="M1 12s4-8 11-8 11 8 11 8-4 8-11 8-11-8-11-8z"/><circle cx="12" cy="12" r="3"/>';
}

/* -- Connect ---------------------------------------------------- */
function wifiDoConnect() {
  const manual = $('wifiManualCb').checked;
  const ssid   = manual ? $('wifiManualSSID').value.trim() : wifiSelectedSSID;
  if (!ssid) return;
  const pwEl = $('wifiPwInput');
  const pw   = pwEl.value;
  pwEl.value = '';
  const badge = $('gwModeBadge');
  const wasInSta = badge && badge.textContent.trim() === 'STA';
  wifiShowConnecting(ssid);
  const ctrl = new AbortController();
  const abortT = setTimeout(() => ctrl.abort(), 5000);
  fetch('/api/connect', {
    method: 'POST',
    headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
    body: 'ssid=' + encodeURIComponent(ssid) + '&password=' + encodeURIComponent(pw),
    signal: ctrl.signal
  })
  .then(r => { clearTimeout(abortT); return r.json(); })
  .then(d => {
    if (d.status === 'connecting') {
      wifiPollStatus(ssid, wasInSta);
    } else {
      wifiShowResult(false, '', ssid);
    }
  })
  .catch(err => {
    clearTimeout(abortT);
    wifiShowApDropped(ssid, wasInSta);
  });
}

function wifiPollStatus(ssid, wasInSta) {
  let tries = 0;
  let failStreak = 0;
  clearInterval(wifiPollIv);
  wifiPollIv = setInterval(() => {
    fetch('/api/wifistatus')
      .then(r => r.json())
      .then(d => {
        failStreak = 0;
        if (d.connected) {
          clearInterval(wifiPollIv);
          wifiShowResult(true, d.ip, ssid);
          fetchSys();
        } else if (tries++ > 25) {
          clearInterval(wifiPollIv);
          wifiShowResult(false, '', ssid);
        }
      })
      .catch(() => {
        tries++;
        if (++failStreak >= 4) {
          clearInterval(wifiPollIv);
          wifiShowApDropped(ssid, wasInSta);
        }
      });
  }, 800);
}

function wifiShowApDropped(ssid, isNetworkSwitch) {
  if (socket) socket.close();
  $('wifiScr-scan').style.display       = 'none';
  $('wifiScr-connecting').style.display = 'none';
  $('wifiScr-result').style.display     = '';
  const ico = $('wifiResultIcon');
  ico.style.borderColor = 'var(--wn)';
  ico.style.background  = 'rgba(255,159,67,.1)';
  ico.style.color       = 'var(--wn)';
  ico.innerHTML = '<svg width="22" height="22" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round"><path d="M5 12.55a11 11 0 0 1 14.08 0"/><path d="M1.42 9a16 16 0 0 1 21.16 0"/><path d="M8.53 16.11a6 6 0 0 1 6.95 0"/><line x1="12" y1="20" x2="12.01" y2="20" stroke-width="2.5"/></svg>';
  $('wifiResultTitle').textContent = 'Gateway Connected';
  const ipHint = wifiCachedStaticIp
    ? `<br>Static IP: <a href="http://${esc(wifiCachedStaticIp)}" target="_blank" style="color:var(--ac);font-family:monospace;font-size:12px">${esc(wifiCachedStaticIp)}</a>`
    : '';
  const body = isNetworkSwitch
    ? `Gateway is switching to <strong>${esc(ssid)}</strong>.<br>` +
      `Connect this device to <strong>${esc(ssid)}</strong>, then open:<br>`
    : `AP was turned off &mdash; gateway joined <strong>${esc(ssid)}</strong>.<br>` +
      `Connect this device to <strong>${esc(ssid)}</strong>, then open:<br>`;
  $('wifiResultSub').innerHTML =
    body +
    `<a href="http://telemeter.local" target="_blank" style="color:var(--ac);font-family:monospace;font-size:12px">http://telemeter.local</a>` +
    ipHint +
    `<br><br><span style="color:var(--txd);font-size:10px">If unreachable, hold the BOOT button and power-cycle to restore AP mode.</span>`;
  const backBtn = $('wifiBackBtn');
  backBtn.innerHTML = 'Close';
  backBtn.onclick = () => toggleWifiDrawer();
}

function wifiCancelConnect() {
  clearInterval(wifiPollIv);
  fetch('/api/disconnect').catch(() => {});
  wifiShowScan();
}



/* -- Static IP -------------------------------------------------- */
function wifiLoadStaticIp() {
  fetch('/api/staticip')
    .then(r => r.json())
    .then(d => {
      $('wifiStaticIpCb').checked = d.enabled;
      $('wifiStaticIp').value  = d.ip      || '';
      $('wifiStaticGw').value  = d.gateway || '';
      $('wifiStaticSn').value  = d.subnet  || '';
      $('wifiStaticDns').value = d.dns     || '';
      wifiCachedStaticIp = d.enabled ? d.ip : '';
      wifiStaticSaved = true; // fields now reflect persisted server state
      wifiToggleStaticIp();
    })
    .catch(() => {});
}

function wifiToggleStaticIp() {
  const enabled = $('wifiStaticIpCb').checked;
  $('wifiStaticIpSection').style.display = enabled ? '' : 'none';
  if (enabled && !$('wifiStaticIp').value) $('wifiStaticIp').focus();
  wifiValidateConnect();
}

function wifiSaveStaticIp() {
  const ip  = $('wifiStaticIp').value.trim();
  const gw  = $('wifiStaticGw').value.trim();
  const sn  = $('wifiStaticSn').value.trim();
  const dns = $('wifiStaticDns').value.trim();
  fetch('/api/staticip', {
    method: 'POST',
    headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
    body: 'ip=' + encodeURIComponent(ip) +
          '&gateway=' + encodeURIComponent(gw) +
          '&subnet='  + encodeURIComponent(sn) +
          '&dns='     + encodeURIComponent(dns)
  })
  .then(r => r.json())
  .then(d => {
    const saveErr = $('wifiStaticSaveErr');
    if (d.ok) {
      wifiCachedStaticIp = ip;
      wifiStaticSaved = true;
      saveErr.textContent = ''; saveErr.style.display = 'none';
      const btn = $('wifiStaticSaveBtn');
      const prev = btn.textContent;
      btn.textContent = 'Saved';
      btn.disabled = true;
      setTimeout(() => { btn.textContent = prev; btn.disabled = false; }, 2000);
      wifiValidateConnect();
    } else {
      saveErr.textContent = 'Rejected by device — check address format.';
      saveErr.style.display = '';
    }
  })
  .catch(() => {
    const saveErr = $('wifiStaticSaveErr');
    saveErr.textContent = 'Save failed — check connection.';
    saveErr.style.display = '';
  });
}

function wifiClearStaticIp() {
  fetch('/api/staticip/clear')
    .then(r => r.json())
    .then(() => {
      wifiCachedStaticIp = '';
      $('wifiStaticIpCb').checked = false;
      $('wifiStaticIp').value  = '';
      $('wifiStaticGw').value  = '';
      $('wifiStaticSn').value  = '';
      $('wifiStaticDns').value = '';
      ['wifiStaticIp','wifiStaticGw','wifiStaticSn','wifiStaticDns'].forEach(id => {
        $(id).classList.remove('err');
        const h = $(id + 'Err'); if (h) { h.textContent = ''; h.style.display = 'none'; }
      });
      $('wifiStaticGwWarn').textContent = ''; $('wifiStaticGwWarn').style.display = 'none';
      $('wifiStaticSaveErr').textContent = ''; $('wifiStaticSaveErr').style.display = 'none';
      wifiStaticSaved = true; // cleared = DHCP will be used, no unsaved config
      wifiToggleStaticIp();
    })
    .catch(() => {});
}

/* -- Forget ----------------------------------------------------- */
function wifiDoForget() {
  hideConfirm('cfmForget');
  fetch('/api/forget')
    .then(() => {
      // Clear selection state
      wifiSelectedSSID = '';
      $('wifiNetList').innerHTML = '<div style="text-align:center;padding:18px 0;color:var(--txd);font-size:11px;font-family:monospace">Credentials cleared. Scan to reconnect.</div>';
      $('wifiCredSection').style.display = 'none';
      $('wifiManualCb').checked = false;
      wifiToggleManual();
      // Reload after a short delay so the gateway has time to restore the AP
      setTimeout(() => location.reload(), 1500);
    })
    .catch(() => {});
}

/* -- Features ---------------------------------------------------- */
let gwFeatures = {};

async function fetchFeatures() {
  try {
    const r = await fetch('/api/features');
    const d = await r.json();
    gwFeatures = d;
    const enc = !!d.encryption;
    const el = id => $(id);
    if (el('encBadge'))   el('encBadge').style.display   = enc ? '' : 'none';
    if (el('provSection')) el('provSection').style.display = enc ? '' : 'none';
  } catch(e) {}
}

/* -- Provision --------------------------------------------------- */
let provPollIv = null;

async function doProvision() {
  const btn = $('provBtn'), st = $('provStatus');
  if (!btn || btn.disabled) return;
  btn.disabled = true;
  if (st) { st.style.color = 'var(--txd)'; st.textContent = 'Waiting for card…'; }
  try {
    const r = await fetch('/api/provision', { method: 'POST' });
    if (r.status === 409) {
      if (st) { st.style.color = 'var(--wn)'; st.textContent = 'Already in progress'; }
      btn.disabled = false; return;
    }
  } catch(e) {
    if (st) { st.style.color = 'var(--dg)'; st.textContent = 'Request failed'; }
    btn.disabled = false; return;
  }
  clearInterval(provPollIv);
  provPollIv = setInterval(async () => {
    try {
      const r = await fetch('/api/provision/status');
      const d = await r.json();
      if (d.status === 'ok') {
        clearInterval(provPollIv);
        if (st) { st.style.color = 'var(--ac)'; st.textContent = 'Key written'; }
        setTimeout(() => { if (st) st.textContent = ''; btn.disabled = false; }, 3000);
      } else if (d.status === 'fail') {
        clearInterval(provPollIv);
        if (st) { st.style.color = 'var(--dg)'; st.textContent = 'Write failed — try again'; }
        btn.disabled = false;
      }
    } catch(e) { clearInterval(provPollIv); btn.disabled = false; }
  }, 500);
}

/* -- Reboot ------------------------------------------------------ */
async function execReboot() {
  hideConfirm('cfmReboot');
  try { await fetch('/api/reboot', { method: 'POST' }); } catch(e) {}
  sT('gwUptime', 'Rebooting…');
  // Wait for the ESP32 to go offline, then poll until it responds and reload.
  setTimeout(() => {
    const poll = setInterval(async () => {
      try {
        await fetch('/api/features');
        clearInterval(poll);
        location.reload();
      } catch(e) {}
    }, 1500);
  }, 2000);
}

/* -- Rule load-out presets --------------------------------------- */
const LS_LOADOUTS     = 'pt-loadouts';
const LS_NODE_LOADOUT = 'pt-node-loadouts';

function getLoadouts(){try{return JSON.parse(localStorage.getItem(LS_LOADOUTS)||'[]');}catch(e){return[];}}
function saveLoadouts(a){localStorage.setItem(LS_LOADOUTS,JSON.stringify(a));}
function getNodeLoadoutMap(){try{return JSON.parse(localStorage.getItem(LS_NODE_LOADOUT)||'{}');}catch(e){return {};}}
function setNodeLoadout(nodeId,name){const m=getNodeLoadoutMap();m[nodeId]={name,at:Date.now()};localStorage.setItem(LS_NODE_LOADOUT,JSON.stringify(m));}

function renderLoadoutSelector(){
  const sel=$('loadoutSelect');if(!sel)return;
  const prev=sel.value;
  const los=getLoadouts();
  sel.innerHTML='<option value="">— Select preset —</option>';
  los.forEach(l=>{const o=document.createElement('option');o.value=l.name;o.textContent=l.name+' ('+l.rules.length+' rule'+(l.rules.length!==1?'s':'')+')';sel.appendChild(o);});
  if(prev&&los.find(l=>l.name===prev))sel.value=prev;
}

function updateLoadoutStatus(){
  const el=$('loadoutLastApplied');if(!el||!cNid)return;
  const entry=getNodeLoadoutMap()[cNid];
  if(!entry){el.textContent='';return;}
  const ago=Math.round((Date.now()-entry.at)/1000);
  const t=ago<60?ago+'s ago':ago<3600?Math.floor(ago/60)+'m ago':Math.floor(ago/3600)+'h ago';
  el.textContent='Last: "'+entry.name+'" · '+t;
}

function renderLoadoutManager(){
  const el=$('loadoutManagerList');if(!el)return;
  const los=getLoadouts();
  if(!los.length){
    el.innerHTML='<div style="text-align:center;padding:14px 0;color:var(--txd);font-size:11px;font-family:\'JetBrains Mono\',monospace;line-height:1.7">No presets saved.<br>Build rules on a node,<br>then press <strong>Save Preset</strong>.</div>';
    return;
  }
  el.innerHTML='';
  los.forEach(lo=>{
    const pIcons=[lo.rules.filter(r=>r.type==='protection').length?'⚡':'',lo.rules.filter(r=>r.type==='schedule').length?'⏱':'',lo.rules.filter(r=>r.type==='default').length?'◎':''].filter(Boolean).join(' ');
    const row=document.createElement('div');row.className='loadout-row';
    row.innerHTML=`<div style="flex:1;min-width:0"><div class="M" style="font-size:12px;font-weight:600;white-space:nowrap;overflow:hidden;text-overflow:ellipsis">${esc(lo.name)}</div><div style="font-size:10px;color:var(--txd);font-family:'JetBrains Mono',monospace">${lo.rules.length} rule${lo.rules.length!==1?'s':''} ${pIcons}</div></div><button class="btn btn-ghost M" style="padding:2px 9px;font-size:10px;color:var(--dg);border-color:rgba(255,56,96,.25)" onclick="deleteLoadout(${JSON.stringify(lo.name)})">Delete</button>`;
    el.appendChild(row);
  });
}

function saveCurrentAsLoadout(){
  const name=prompt('Preset name:','');
  if(!name||!name.trim())return;
  const nm=name.trim();
  const rules=buildRulesFromBuilder();
  const los=getLoadouts();
  const idx=los.findIndex(l=>l.name===nm);
  if(idx>=0){if(!confirm('Overwrite "'+nm+'"?'))return;los[idx]={name:nm,rules,savedAt:Date.now()};}
  else{los.push({name:nm,rules,savedAt:Date.now()});}
  saveLoadouts(los);
  renderLoadoutSelector();renderLoadoutManager();
  const sel=$('loadoutSelect');if(sel)sel.value=nm;
}

function deleteLoadout(name){
  if(!confirm('Delete preset "'+name+'"?'))return;
  saveLoadouts(getLoadouts().filter(l=>l.name!==name));
  renderLoadoutSelector();renderLoadoutManager();
}

function loadLoadoutIntoBuilder(){
  const sel=$('loadoutSelect');if(!sel||!sel.value)return;
  const lo=getLoadouts().find(l=>l.name===sel.value);if(!lo)return;
  populateRuleBuilder(lo.rules);
}

function applyLoadoutDirect(){
  const sel=$('loadoutSelect');if(!cNid||!sel||!sel.value)return;
  const lo=getLoadouts().find(l=>l.name===sel.value);if(!lo)return;
  if(lo.rules.length>8){alert('Preset exceeds 8 rules.');return;}
  doSetRules(cNid,lo.rules);
  setNodeLoadout(cNid,sel.value);
  updateLoadoutStatus();
}

/* -- AutoRule builder -------------------------------------------- */
const RULE_FIELD_LABELS=['Voltage','Current','Power','Energy','Frequency','Power Factor'];
const RULE_FIELD_UNITS=['V','A','W','Wh','Hz',''];
const RULE_OPS=[{v:0,l:'> GT'},{v:1,l:'< LT'},{v:2,l:'≥ GE'},{v:3,l:'≤ LE'}];

function doSetRules(nodeId,rules){wsSend({cmd:'set_rules',node:nodeId,rules});}

function loadNodeRules(id){
  fetch('/api/node/'+id+'/rules')
    .then(r=>r.json())
    .then(data=>{currentRules[id]=data;if(cNid===id)populateRuleBuilder(data);})
    .catch(()=>{});
}

function minutesToTime(m){return pad2(Math.floor(m/60))+':'+pad2(m%60);}
function timeToMinutes(t){const[h,m]=t.split(':').map(Number);return h*60+(m||0);}

function populateRuleBuilder(rules){
  builderScheds=[];builderProts=[];
  let defAction='off';
  (rules||[]).forEach(r=>{
    if(r.type==='default'){defAction=r.action;}
    else if(r.type==='schedule'){builderScheds.push({onTime:r.param_a,offTime:r.param_b,action:r.action,enabled:r.enabled!==false});}
    else if(r.type==='protection'){builderProts.push({field:r.field,op:r.op,threshold:r.param_a,hysteresis:r.param_b,action:r.action,enabled:r.enabled!==false});}
  });
  const sel=$('ruleDefaultAction');
  if(sel)sel.value=(rules&&rules.length)?defAction:'off';
  renderRuleSchedList();renderRuleProtList();
}

function renderRuleSchedList(){
  const el=$('ruleSchedList');if(!el)return;
  el.innerHTML='';
  builderScheds.forEach((s,i)=>{
    const row=document.createElement('div');row.className='rule-row M';
    row.innerHTML=`<input type="time" class="form-input M" value="${minutesToTime(s.onTime)}" style="flex:1;min-width:0;margin:0" onchange="builderScheds[${i}].onTime=timeToMinutes(this.value)"><span style="color:var(--txd);padding:0 3px">—</span><input type="time" class="form-input M" value="${minutesToTime(s.offTime)}" style="flex:1;min-width:0;margin:0" onchange="builderScheds[${i}].offTime=timeToMinutes(this.value)"><select class="form-input M" style="width:52px;margin:0;padding:4px" onchange="builderScheds[${i}].action=this.value"><option value="on"${s.action==='on'?' selected':''}>ON</option><option value="off"${s.action==='off'?' selected':''}>OFF</option></select><button class="btn btn-ghost M" style="padding:2px 7px;font-size:11px;margin:0" onclick="builderScheds.splice(${i},1);renderRuleSchedList()">×</button>`;
    el.appendChild(row);
  });
}

function renderRuleProtList(){
  const el=$('ruleProtList');if(!el)return;
  el.innerHTML='';
  builderProts.forEach((p,i)=>{
    const row=document.createElement('div');row.className='rule-row M';
    const fOpts=RULE_FIELD_LABELS.map((f,fi)=>`<option value="${fi}"${fi===p.field?' selected':''}>${f}</option>`).join('');
    const oOpts=RULE_OPS.map(o=>`<option value="${o.v}"${o.v===p.op?' selected':''}>${o.l}</option>`).join('');
    row.innerHTML=`<select class="form-input M" style="flex:1;min-width:0;margin:0;padding:4px" onchange="builderProts[${i}].field=parseInt(this.value)">${fOpts}</select><select class="form-input M" style="width:58px;margin:0;padding:4px" onchange="builderProts[${i}].op=parseInt(this.value)">${oOpts}</select><input type="number" class="thr-input M" value="${p.threshold}" style="width:64px;margin:0" onchange="builderProts[${i}].threshold=parseInt(this.value)||0" placeholder="Thresh"><span style="color:var(--txd);padding:0 3px">→</span><select class="form-input M" style="width:46px;margin:0;padding:4px" onchange="builderProts[${i}].action=this.value"><option value="off"${p.action==='off'?' selected':''}>OFF</option><option value="on"${p.action==='on'?' selected':''}>ON</option></select><button class="btn btn-ghost M" style="padding:2px 7px;font-size:11px;margin:0" onclick="builderProts.splice(${i},1);renderRuleProtList()">×</button>`;
    el.appendChild(row);
  });
}

function ruleAddSched(){builderScheds.push({onTime:480,offTime:1080,action:'on',enabled:true});renderRuleSchedList();}
function ruleAddProt(){builderProts.push({field:2,op:0,threshold:2000,hysteresis:200,action:'off',enabled:true});renderRuleProtList();}

function buildRulesFromBuilder(){
  const rules=[];
  const defAction=$('ruleDefaultAction').value;
  rules.push({type:'default',action:defAction,enabled:true});
  builderScheds.forEach(s=>rules.push({type:'schedule',action:s.action,enabled:s.enabled,onTime:s.onTime,offTime:s.offTime}));
  builderProts.forEach(p=>rules.push({type:'protection',action:p.action,enabled:p.enabled,field:p.field,op:p.op,threshold:p.threshold,hysteresis:p.hysteresis||0}));
  return rules;
}

function doApplyRules(){
  if(!cNid)return;
  const rules=buildRulesFromBuilder();
  if(rules.length>8){alert('Maximum 8 rules total (including default).');return;}
  doSetRules(cNid,rules);
  // If the selected preset was loaded and applied unchanged, record it
  const sel=$('loadoutSelect');
  if(sel&&sel.value){setNodeLoadout(cNid,sel.value);updateLoadoutStatus();}
}

/* -- Init -------------------------------------------------------- */
function init() {
  document.addEventListener('DOMContentLoaded',()=>{
    applyTheme(getTheme());
    $('costRateIn').value=costRate;
    $('btnClearLog')?.addEventListener('click',()=>{const b=$('serialLogBox');if(b)b.innerHTML='';});
    buildLogFilterRow();
    connectWS();
    route();
    setInterval(fetchSys,10000);
    fetchSys();
    fetchFeatures();
    renderLoadoutManager();
  });
}