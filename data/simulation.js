/* -----------------------------------------------------------------
   SIMULATION ENGINE
   Replaces connectWS, wsSend, fetchSys, fetchFeatures, apLoadInfo,
   dashLoadSecure, wifiLoadStaticIp, doSyncTime, execReboot,
   wifiDoScan, wifiDoConnect, wifiCancelConnect, wifiDoForget, init.
   All other UI functions are identical to production.
   ----------------------------------------------------------------- */

/* -- Node profiles ------------------------------------------------ */
// clfClass: 0=Resistive 1=Capacitive 2=Motor 3=Fan 4=SMPS 5=Lighting 6=Off(LOAD_NONE)
// clfConf : 0-7 base confidence; undefined = no classifier (PZEM-class node)
// Note: classId 6 (Off) is emitted automatically when relayState===0; clfClass never stores 6.
const SIM_PROFILES = [
  { id:1, label:'Refrigerator',   nominalPower:120,  idlePower:5,   cyclePeriod:600,  cycleOnFrac:0.6,  pf:0.72, pfNoise:0.04, voltNominal:220, freqNominal:60, rssiBase:-58, alarmThreshold:500,  relayState:1, relayMode:0, clfClass:2, clfConf:6 },
  { id:2, label:'AC Unit',        nominalPower:1450, idlePower:30,  cyclePeriod:900,  cycleOnFrac:0.75, pf:0.82, pfNoise:0.03, voltNominal:220, freqNominal:60, rssiBase:-71, alarmThreshold:2000, relayState:1, relayMode:0, clfClass:3, clfConf:5 },
  { id:3, label:'Outdoor Lights', nominalPower:85,   idlePower:0,   cyclePeriod:3600, cycleOnFrac:1.0,  pf:0.95, pfNoise:0.01, voltNominal:220, freqNominal:60, rssiBase:-44, alarmThreshold:200,  relayState:1, relayMode:0, clfClass:5, clfConf:7 },
  { id:4, label:'PC',             nominalPower:320,  idlePower:95,  cyclePeriod:120,  cycleOnFrac:0.55, pf:0.88, pfNoise:0.05, voltNominal:220, freqNominal:60, rssiBase:-65, alarmThreshold:600,  relayState:1, relayMode:0, clfClass:4, clfConf:6 },
  { id:5, label:'Heater',         nominalPower:2000, idlePower:0,   cyclePeriod:600,  cycleOnFrac:0.5,  pf:0.99, pfNoise:0.01, voltNominal:220, freqNominal:60, rssiBase:-88, alarmThreshold:3000, relayState:0, relayMode:0, _permanentlyOffline:true },
  { id:6, label:'Water Pump',    nominalPower:750,  idlePower:0,   cyclePeriod:300,  cycleOnFrac:0.4,  pf:0.78, pfNoise:0.03, voltNominal:220, freqNominal:60, rssiBase:-67, alarmThreshold:1000, relayState:1, relayMode:0 },
  { id:7, label:'Storage Light', nominalPower:5, idlePower:0, cyclePeriod: 3600, cycleOnFrac: 1.0, pf:0.84, pfNoise: 0.05, voltNominal: 220, freqNominal: 60, rssiBase:-89, alarmThreshold:50, relayState:1, relayMode:0, clfClass:6, clfConf:7 }
];

const SIM_TICK_MS  = 3000;
const SIM_HIST_LEN = 120;
// Ticks before a classifier node exits PENDING (mirrors the 5-sample AFE warmup)
const SIM_CLF_WARMUP = 4;

const simState = SIM_PROFILES.map(p => ({
  ...p,
  online:        p._permanentlyOffline ? false : true,
  energy:        Math.random() * 800 + 50,
  age:           0,
  pending:       false,
  hasSched:      false,
  schedStart:    '08:00',
  schedEnd:      '17:00',
  schedState:    0,
  _t:            Math.random() * p.cyclePeriod,
  _offlineTimer: 0,
  _offlineIn:    p._permanentlyOffline ? Infinity : randomOfflineDelay(),
  _clfTick:      0,
  _hist:         { p:[], v:[], i:[] },
}));

let simUptime  = 0;
let simHeap    = 180 + Math.random() * 40;
let simRSSI    = -52 + Math.floor(Math.random() * 10);
let simSSID    = 'HomeNetwork_5G';
let simSfCount = 0;
const SIM_LOGS = [
  ()=>`[GW-TDMA] Superframe #${simSfCount} ch=0 beacon TX ok`,
  ()=>`[GW-UL] Slot1 V=220.4 A=1.234 W=271.9 Wh=8732 RSSI=-72`,
  ()=>`[GW-UL] Slot2 V=219.8 A=3.012 W=660.4 Wh=14201 RSSI=-85`,
  ()=>`[GW-CW] No join requests this window`,
  ()=>`[GW-CW] JoinReq UID=0xAB12 fw=2`,
  ()=>`[GW-CW] Assigned slot 3 to UID=0xAB12 epoch=1`,
  ()=>`[WIFI] STA connected | IP: 192.168.1.100 | RSSI: -63 dBm`,
  ()=>`[FRAM] Slot 1: saved label "Refrigerator"`,
  ()=>`[WEB] Server started on port 80`,
  ()=>`[CRYPTO] Encrypted mode active`,
  ()=>`[CRYPTO] New key generated: 3F9A1C...`,
  ()=>`[RFID] PN532 OK (fw=0x07060700, SDA=32 SCL=33)`,
  ()=>`[RFID] Key written to card (block 4)`,
  ()=>`[RFID] No card detected (5 s timeout)`,
  ()=>`[NODE-JOIN] Sent JoinReq UID=0xAB12`,
  ()=>`[NODE-TDMA] Epoch changed (0->1) - re-registering`,
  ()=>`[NODE-TX] V=220.1 A=0.548 W=120.7 seq=${simSfCount}`,
  ()=>`[NODE-DL] Relay manual -> ON`,
  ()=>`[NODE-SCHED] Schedule active (now=14:00)`,
  ()=>`[NODE-RTC] Corrected +2 ms`,
  ()=>`[PZEM] V=220.4 A=1.234 W=271.9 Hz=60.0 PF=0.88`,
];

function randomOfflineDelay(){ return 240 + Math.random() * 480; }

function noise(amp){ return (Math.random()-0.5)*2*amp; }

function simTick(){
  const dt  = SIM_TICK_MS / 1000;
  const now = new Date();
  simState.forEach(s=>{
    s._offlineIn -= dt;
    if(s._offlineIn<=0 && s.online){ s.online=false; s._offlineTimer=4+Math.random()*8; s._clfTick=0; }
    if(!s.online){
      s._offlineTimer-=dt;
      if(s._offlineTimer<=0 && !s._permanentlyOffline){ s.online=true; s._offlineIn=randomOfflineDelay(); }
      s.age=0; return;
    }
    s._clfTick++;
    s.age=Math.round(dt);
    s._t+=dt; if(s._t>s.cyclePeriod)s._t-=s.cyclePeriod;
    const inOn=(s._t/s.cyclePeriod)<s.cycleOnFrac;
    if(!s.relayState){
      s.voltage=s.voltNominal+noise(0.6); s.current=0; s.power=0;
      s.frequency=s.freqNominal+noise(0.06); s.powerFactor=0; s.alarmState=0;
      appendHist(s,0,s.voltage,0); return;
    }
    const raw = inOn ? s.nominalPower+s.nominalPower*0.12*Math.sin(s._t*0.8)+noise(s.nominalPower*0.05)
                     : s.idlePower+noise(s.idlePower*0.1+0.5);
    const p   = Math.max(0,raw);
    const I0  = p/(s.voltNominal*s.pf+0.001);
    const v   = Math.max(195, s.voltNominal-I0*0.3+noise(0.4));
    const pf  = Math.min(1,Math.max(0.5,s.pf+noise(s.pfNoise)));
    const i   = p/(v*pf+0.001);
    const f   = s.freqNominal+0.06*Math.sin(Date.now()/18000)+noise(0.04);
    s.energy += p*(dt/3600);
    s.alarmState = (p>s.alarmThreshold)?1:0;
    s.voltage=parseFloat(v.toFixed(1)); s.current=parseFloat(i.toFixed(3));
    s.power=parseFloat(p.toFixed(1));   s.frequency=parseFloat(f.toFixed(2));
    s.powerFactor=parseFloat(pf.toFixed(2));
    appendHist(s,s.power,s.voltage,s.current);
  });

  const nodes=simState.map(nodeSnapshot);
  nodes.forEach(n=>{ const i=NC.findIndex(x=>x.id===n.id); if(i>=0)Object.assign(NC[i],n); else NC.push(n); });
  if(!cNid)renderGrid(NC);
  if(cNid){ const s=simState.find(x=>x.id===cNid); if(s){ updateDetail(nodeSnapshot(s)); addChartPoint({p:s.power,v:s.voltage,i:s.current,power:s.power,voltage:s.voltage,current:s.current}); } }
  const cnt=simState.filter(s=>s.online).length;
  $('nodeCount').textContent=cnt+' node'+(cnt!==1?'s':'');
  sT('idxCount',simState.length);
  gwTimeSet=true; gwTime=pad2(now.getHours())+':'+pad2(now.getMinutes())+':'+pad2(now.getSeconds());
  updGwTime();
  simUptime+=dt;
  appendLog(SIM_LOGS[simSfCount % SIM_LOGS.length]());
  simSfCount++;
}

function appendHist(s,p,v,i){
  s._hist.p.push(parseFloat(p.toFixed(1)));
  s._hist.v.push(parseFloat(v.toFixed(1)));
  s._hist.i.push(parseFloat(i.toFixed(3)));
  if(s._hist.p.length>SIM_HIST_LEN){s._hist.p.shift();s._hist.v.shift();s._hist.i.shift();}
}

function nodeSnapshot(s){
  // Build classifier object from sim state
  let classifier;
  if(s.clfClass===undefined){
    // PZEM-class node: no classifier hardware
    classifier={supported:false};
  }else if(s.relayState===0){
    // Relay off -- mechanical isolation makes no-load unambiguous; emit LOAD_NONE immediately.
    // Mirrors firmware: LOAD_NONE bypasses the smoother, smoother state is preserved.
    classifier={supported:true,pending:false,classId:6,confidence:7,transient:false};
  }else if(s._clfTick<SIM_CLF_WARMUP){
    // Relay on but smoother window not yet full
    classifier={supported:true,pending:true};
  }else{
    // Active load -- confidence drops during idle phase of load cycle
    const inIdle=(s._t/s.cyclePeriod)>=s.cycleOnFrac;
    const baseConf=inIdle?Math.max(2,s.clfConf-2):s.clfConf;
    const conf=Math.min(7,Math.max(0,Math.round(baseConf+noise(0.8))));
    // Transient flag: edges of on/off cycle transition
    const cyclePhase=s._t/s.cyclePeriod;
    const trans=cyclePhase>0.92||cyclePhase<0.04;
    classifier={supported:true,pending:false,classId:s.clfClass,confidence:conf,transient:trans};
  }
  return {
    id:s.id, label:s.label, online:s.online,
    rssi:s.online?Math.round(s.rssiBase+noise(6)):s.rssiBase,
    voltage:s.voltage||s.voltNominal, current:s.current||0, power:s.power||0,
    energy:parseFloat((s.energy||0).toFixed(1)), frequency:s.frequency||s.freqNominal,
    powerFactor:s.powerFactor||s.pf, relayState:s.relayState, relayMode:s.relayMode,
    schedState:s.schedState, alarmState:s.alarmState||0, age:s.age||0, pending:s.pending||false,
    hasSched:s.hasSched, schedStart:s.schedStart, schedEnd:s.schedEnd,
    classifier,
  };
}

/* -- Overrides ---------------------------------------------------- */

connectWS = function(){
  wsDot(true);
  NC=simState.map(nodeSnapshot);
  renderGrid(NC);
  sT('idxCount',simState.length);
  $('nodeCount').textContent=simState.length+' nodes';
  setInterval(simTick,SIM_TICK_MS);
  simTick();
};

wsSend = function(cmd){
  if(!cmd||!cmd.cmd)return;
  switch(cmd.cmd){
    case 'get_nodes': break;
    case 'relay_manual':{
      const s=simState.find(x=>x.id===cmd.node); if(!s)break;
      s.pending=true; renderGrid(NC);
      setTimeout(()=>{ s.relayState=cmd.state; s.relayMode=0; s.pending=false;
        if(cNid===cmd.node){$('commitBanner').classList.remove('show');updateDetail(nodeSnapshot(s));}
        renderGrid(NC); }, 600+Math.random()*400); break;
    }
    case 'relay_schedule':{
      const s=simState.find(x=>x.id===cmd.node); if(!s)break;
      s.pending=true;
      setTimeout(()=>{
        s.relayMode=1; s.hasSched=true;
        s.schedStart=pad2(cmd.startH)+':'+pad2(cmd.startM);
        s.schedEnd=pad2(cmd.endH)+':'+pad2(cmd.endM);
        const now=new Date(); const nm=now.getHours()*60+now.getMinutes();
        const on=cmd.startH*60+cmd.startM; const off=cmd.endH*60+cmd.endM;
        const inW=on<off?(nm>=on&&nm<off):(nm>=on||nm<off);
        s.schedState=inW?2:1; s.relayState=inW?1:0; s.pending=false;
        if(cNid===cmd.node){$('commitBannerSched').classList.remove('show');updateDetail(nodeSnapshot(s));}
      },700+Math.random()*300); break;
    }
    case 'relay_clear':{
      const s=simState.find(x=>x.id===cmd.node); if(!s)break;
      s.pending=true;
      setTimeout(()=>{ s.relayMode=0; s.hasSched=false; s.schedState=0; s.pending=false;
        if(cNid===cmd.node){$('commitBannerSched').classList.remove('show');updateDetail(nodeSnapshot(s));} },500); break;
    }
    case 'set_threshold':{
      const s=simState.find(x=>x.id===cmd.node); if(!s)break;
      setTimeout(()=>{ s.alarmThreshold=cmd.watts;
        $('thrBanner').classList.remove('show'); $('thrCtrl').classList.remove('frozen');
        if(cNid===cmd.node)updateDetail(nodeSnapshot(s)); },400); break;
    }
    case 'clear_energy':{
      const s=simState.find(x=>x.id===cmd.node); if(s){s.energy=0;} break;
    }
    case 'clear_all_energy':{ simState.forEach(s=>s.energy=0); break; }
    case 'rename':{
      const s=simState.find(x=>x.id===cmd.node); if(!s)break;
      s.label=cmd.name; const n=NC.find(x=>x.id===cmd.node); if(n)n.label=cmd.name;
      if(!cNid)renderGrid(NC); break;
    }
    case 'nudge':{
      const s=simState.find(x=>x.id===cmd.node);
      if(s&&s.online){s.online=false;setTimeout(()=>{s.online=true;},1200);} break;
    }
    case 'set_time': gwTimeSet=true; break;
  }
};

fetchHistory = async function(id){
  const s=simState.find(x=>x.id===id); if(!s||!chart)return;
  const mc=MC[cMet]; const buf=s._hist[mc.k]||[];
  if(!buf.length)return;
  chart.data.labels=buf.map((_,i)=>{ const sec=(buf.length-1-i)*(SIM_TICK_MS/1000); return sec>60?Math.floor(sec/60)+'m':sec+'s'; });
  chart.data.datasets[0].data=[...buf]; chart.update('none');
};

fetchSys = async function(){
  simHeap=Math.max(140,Math.min(220,simHeap+(Math.random()-0.5)*4));
  simRSSI=Math.max(-75,Math.min(-35,simRSSI+Math.round((Math.random()-0.5)*2)));
  sT('gwUptime',fU(Math.round(simUptime)));
  sT('gwHeap',simHeap.toFixed(0)+' KB');
  sT('gwNodes',simState.length+'/'+simState.length);
  sT('fwVer','Simulation Edition');
  gwTimeSet=true; updGwTime();
  updNetPanel({ wifiMode:'sta', ssid:simSSID, wifiRSSI:simRSSI, ip:'192.168.1.42' });
};

fetchFeatures = async function(){
  gwFeatures={};
  const enc=$('provSection');
  if(enc)enc.style.display='none';
  const tt=$('transportTestSection');
  if(tt)tt.style.display='none';
};

apLoadInfo = async function(){
  const name=document.getElementById('apSsidName');
  const status=document.getElementById('apSsidStatus');
  if(name)name.textContent='PowerTelemetry-Setup';
  if(status)status.textContent='· Open';
  const inp=document.getElementById('apPwInput');
  if(inp)inp.placeholder='Leave blank for open network';
};

dashLoadSecure = async function(){
  const btn=document.getElementById('dashLogoutBtn');
  if(btn)btn.style.display='none';
  const inp=document.getElementById('dashPwInput');
  if(inp)inp.placeholder='Simulation mode -- settings not persisted';
};

wifiLoadStaticIp = function(){
  const cb=$('wifiStaticIpCb');
  if(cb)cb.checked=false;
  wifiStaticSaved=true;
  wifiToggleStaticIp();
};

doSyncTime = function(){
  gwTimeSet=true;
  const btn=$('syncBtn'); if(!btn)return;
  btn.classList.add('ac-active');
  const prev=btn.innerHTML;
  btn.innerHTML='<svg width="12" height="12" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round"><polyline points="20 6 9 17 4 12"/></svg> Synced';
  clearTimeout(btn._t); btn._t=setTimeout(()=>{ btn.innerHTML=prev; btn.classList.remove('ac-active'); },2500);
};

execReboot = async function(){
  hideConfirm('cfmReboot');
  sT('gwUptime','Rebooting...');
  simUptime=0;
  await new Promise(r=>setTimeout(r,1500));
  sT('gwUptime',fU(0));
};

wifiDoScan = function(){
  const btn=$('wifiScanBtn'); const list=$('wifiNetList');
  btn.classList.add('spinning'); btn.disabled=true;
  list.innerHTML='<div style="text-align:center;padding:18px 0;color:var(--txd);font-size:11px;font-family:monospace">Scanning...</div>';
  setTimeout(()=>{
    try{
      wifiRenderNetworks([
        {ssid:'HomeNetwork_5G',  rssi:-42, secure:true},
        {ssid:'PLDT_FIBER_001A', rssi:-61, secure:true},
        {ssid:'AndroidAP',      rssi:-73, secure:true},
        {ssid:'GuestWifi',      rssi:-55, secure:false},
        {ssid:'DIRECT-Printer', rssi:-80, secure:false},
      ]);
    }finally{ btn.classList.remove('spinning'); btn.disabled=false; }
  },900);
};

wifiDoConnect = function(){
  const manual=$('wifiManualCb').checked;
  const ssid=manual?$('wifiManualSSID').value.trim():wifiSelectedSSID;
  if(!ssid)return;
  wifiShowConnecting(ssid);
  let tries=0;
  clearInterval(wifiPollIv);
  wifiPollIv=setInterval(()=>{
    tries++;
    if(tries>=4){
      clearInterval(wifiPollIv);
      if(ssid==='FailureSSID'){
        wifiShowResult(false,'',ssid); fetchSys();
      }else{
        simSSID=ssid; simRSSI=-52;
        wifiShowResult(true,'192.168.1.42',ssid); fetchSys();
      }
    }
  },800);
};

wifiCancelConnect = function(){
  clearInterval(wifiPollIv);
  wifiShowScan();
};

wifiDoForget = function(){
  hideConfirm('cfmForget');
  simSSID='PowerTelemetry-Setup'; simRSSI=0;
  wifiSelectedSSID='';
  $('wifiNetList').innerHTML='<div style="text-align:center;padding:18px 0;color:var(--txd);font-size:11px;font-family:monospace">Credentials cleared. Scan to reconnect.</div>';
  $('wifiCredSection').style.display='none';
  $('wifiManualCb').checked=false;
  wifiToggleManual();
  setTimeout(fetchSys,500);
};

function injectSimBanner(){
  const hdr=document.querySelector('.hdr-r');
  const badge=document.createElement('span');
  badge.className='hdr-badge M';
  badge.style.cssText='border-color:rgba(255,159,67,.4);color:var(--wn);background:rgba(255,159,67,.08)';
  badge.innerHTML='<svg width="10" height="10" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" style="margin-right:4px;vertical-align:middle"><polygon points="13 2 3 14 12 14 11 22 21 10 12 10 13 2"/></svg>SIM';
  hdr.prepend(badge);
  document.title='PowerTelemetry · Dashboard · SIM';
}

// Completely replace init: bypass the auth flow, call startApp() directly.
// The real init does an async /api/authstatus fetch that fails without a gateway,
// which leaves the auth overlay blocking the entire UI.
init = function(){
  document.addEventListener('DOMContentLoaded',()=>{
    applyTheme(getTheme());
    $('costRateIn').value=costRate;
    $('btnClearLog')?.addEventListener('click',()=>{ const b=$('serialLogBox'); if(b)b.innerHTML=''; });
    buildLogFilterRow();
    hideLoginOverlay();
    injectSimBanner();
    startApp();
  });
};
