/* -----------------------------------------------------------------
   SIMULATION ENGINE
   Replaces connectWS, wsSend, fetchHistory, execReboot,
   wifiDoScan, wifiDoConnect, wifiDoForget, doSyncTime, doLogout,
   and init.  All remaining app.js functions run through simFetch --
   a central fetch interceptor that mocks every /api/ endpoint so
   real app.js code paths execute exactly as on a live gateway.
   ----------------------------------------------------------------- */

/* -- Node profiles ------------------------------------------------ */
// clfClass: 0=Resistive 1=Capacitive 2=Motor 3=Fan 4=SMPS 5=Lighting 6=Off(LOAD_NONE)
// clfConf : 0-7 base confidence; undefined = no classifier (PZEM-class node)
// Note: classId 6 (Off) is emitted automatically when relayState===0; clfClass never stores 6.
// knnSettleTicks : ticks before IDENTIFIED (default 4 = 12 s)
// knnDistSqRange : [min, max] for randomised dist^2 -- low = strong, high = weak
// knnDropOnTransient: true -> drops back to SETTLING at cycle edges
// knnUnidentified   : true -> load not in trained model, stays UNIDENTIFIED
const SIM_PROFILES = [
  { id:1, label:'Refrigerator',   nominalPower:120,  idlePower:5,   cyclePeriod:600,  cycleOnFrac:0.6,  pf:0.72, pfNoise:0.04, voltNominal:220, freqNominal:60, rssiBase:-58, alarmThreshold:500,  relayState:1, relayMode:0, clfClass:2, clfConf:6, knnSettleTicks:4, knnDistSqRange:[0.007,0.032] },
  { id:2, label:'AC Unit',        nominalPower:1450, idlePower:30,  cyclePeriod:900,  cycleOnFrac:0.75, pf:0.82, pfNoise:0.03, voltNominal:220, freqNominal:60, rssiBase:-71, alarmThreshold:2000, relayState:1, relayMode:0, clfClass:3, clfConf:5, knnSettleTicks:5, knnDistSqRange:[0.018,0.065], knnDropOnTransient:true },
  { id:3, label:'Outdoor Lights', nominalPower:85,   idlePower:0,   cyclePeriod:3600, cycleOnFrac:1.0,  pf:0.95, pfNoise:0.01, voltNominal:220, freqNominal:60, rssiBase:-44, alarmThreshold:200,  relayState:1, relayMode:0, clfClass:5, clfConf:7, knnSettleTicks:3, knnDistSqRange:[0.003,0.018] },
  { id:4, label:'PC',             nominalPower:320,  idlePower:95,  cyclePeriod:120,  cycleOnFrac:0.55, pf:0.88, pfNoise:0.05, voltNominal:220, freqNominal:60, rssiBase:-65, alarmThreshold:600,  relayState:1, relayMode:0, clfClass:4, clfConf:6, knnSettleTicks:6, knnDistSqRange:[0.045,0.130], knnDropOnTransient:true },
  { id:5, label:'Heater',         nominalPower:2000, idlePower:0,   cyclePeriod:600,  cycleOnFrac:0.5,  pf:0.99, pfNoise:0.01, voltNominal:220, freqNominal:60, rssiBase:-88, alarmThreshold:3000, relayState:0, relayMode:0, _permanentlyOffline:true },
  { id:6, label:'Water Pump',     nominalPower:750,  idlePower:0,   cyclePeriod:300,  cycleOnFrac:0.4,  pf:0.78, pfNoise:0.03, voltNominal:220, freqNominal:60, rssiBase:-67, alarmThreshold:1000, relayState:1, relayMode:0, knnUnidentified:true },
  { id:7, label:'Storage Light',  nominalPower:5,    idlePower:0,   cyclePeriod:3600, cycleOnFrac:1.0,  pf:0.84, pfNoise:0.05, voltNominal:220, freqNominal:60, rssiBase:-89, alarmThreshold:50,   relayState:1, relayMode:0, clfClass:1, clfConf:7, knnSettleTicks:8, knnDistSqRange:[0.085,0.250] }
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
  _knnTick:      0,
  _knnState:     'UNIDENTIFIED',
  _knnDistSq:    0,
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
  ()=>`[KNN] Node 1 IDENTIFIED: "Refrigerator" (dist2=0.031)`,
  ()=>`[KNN] Node 2 SETTLING -- load transient in progress`,
  ()=>`[KNN] Node 4 weak match dist2=${(0.045+Math.random()*0.085).toFixed(3)} (above confidence threshold)`,
  ()=>`[KNN] Node 6 UNIDENTIFIED -- no match in model`,
  ()=>`[KNN] Node 7 IDENTIFIED: "Storage Light" dist2=${(0.085+Math.random()*0.165).toFixed(3)} (weak)`,
  ()=>`[KNN] Training tick: samples=${Math.floor(Math.random()*60)+10}`,
  ()=>`[KNN] Model persisted to FRAM (${SIM_PROFILES.filter(p=>!p._permanentlyOffline).length} profiles)`,
];

function randomOfflineDelay(){ return 240 + Math.random() * 480; }

function noise(amp){ return (Math.random()-0.5)*2*amp; }

function simTick(){
  const dt  = SIM_TICK_MS / 1000;
  const now = new Date();
  simState.forEach(s=>{
    s._offlineIn -= dt;
    if(s._offlineIn<=0 && s.online){ s.online=false; s._offlineTimer=4+Math.random()*8; s._clfTick=0; s._knnTick=0; s._knnState='UNIDENTIFIED'; s._knnDistSq=0; }
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
      s._knnTick=0; s._knnState='UNIDENTIFIED'; s._knnDistSq=0;
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
    s._knnTick++;
    const knnSettle=s.knnSettleTicks||4;
    const knnPhase=s._t/s.cyclePeriod;
    const knnTransient=(knnPhase>0.93||knnPhase<0.03)&&s.knnDropOnTransient;
    if(s._knnTick<knnSettle||knnTransient){
      s._knnState='SETTLING';s._knnDistSq=0;
    }else if(s.knnUnidentified){
      s._knnState='UNIDENTIFIED';s._knnDistSq=0;
    }else{
      const[dMin,dMax]=s.knnDistSqRange||[0.008,0.055];
      s._knnState='IDENTIFIED';
      s._knnDistSq=parseFloat((dMin+Math.random()*(dMax-dMin)).toFixed(3));
    }
  });

  const nodes=simState.map(nodeSnapshot);
  nodes.forEach(n=>{ const i=NC.findIndex(x=>x.id===n.id); if(i>=0)Object.assign(NC[i],n); else NC.push(n); recordEnergyAnchor(n.id,n.energy||0); });
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
  let classifier;
  if(s.clfClass===undefined){
    classifier={supported:false};
  }else if(s.relayState===0){
    classifier={supported:true,pending:false,classId:6,confidence:7,transient:false};
  }else if(s._clfTick<SIM_CLF_WARMUP){
    classifier={supported:true,pending:true};
  }else{
    const inIdle=(s._t/s.cyclePeriod)>=s.cycleOnFrac;
    const baseConf=inIdle?Math.max(2,s.clfConf-2):s.clfConf;
    const conf=Math.min(7,Math.max(0,Math.round(baseConf+noise(0.8))));
    const cyclePhase=s._t/s.cyclePeriod;
    const trans=cyclePhase>0.92||cyclePhase<0.04;
    classifier={supported:true,pending:false,classId:s.clfClass,confidence:conf,transient:trans};
  }
  const knn={
    state:  s._knnState||'UNIDENTIFIED',
    label:  s._knnState==='IDENTIFIED'?(s.label||''):'',
    distSq: s._knnDistSq||0,
  };
  return {
    id:s.id, label:s.label, online:s.online,
    rssi:s.online?Math.round(s.rssiBase+noise(6)):s.rssiBase,
    voltage:s.voltage||s.voltNominal, current:s.current||0, power:s.power||0,
    energy:parseFloat((s.energy||0).toFixed(1)), frequency:s.frequency||s.freqNominal,
    powerFactor:s.powerFactor||s.pf, relayState:s.relayState, relayMode:s.relayMode,
    schedState:s.schedState, alarmState:s.alarmState||0, age:s.age||0, pending:s.pending||false,
    hasSched:s.hasSched, schedStart:s.schedStart, schedEnd:s.schedEnd,
    classifier, knn,
  };
}

/* -- KNN simulation state ---------------------------------------- */
let simKnnLoaded       = true;
let simKnnFromEmbed    = false;
let simKnnProfileCount = SIM_PROFILES.filter(p=>!p._permanentlyOffline).length;
const simKnnSessions   = {};   // nodeId (int) -> session object
let simProvisionStarted = null;

/* -- Central fetch interceptor ----------------------------------- */
// Builds a Response-like object that satisfies .json() / .blob() / .text()
function simOkResp(data){
  return Promise.resolve({
    ok: true, status: 200,
    json: ()=>Promise.resolve(data),
    text: ()=>Promise.resolve(JSON.stringify(data)),
    blob: ()=>Promise.resolve(new Blob([JSON.stringify(data)],{type:'application/json'})),
  });
}

function simFetch(url, opts){
  const method  = ((opts&&opts.method)||'GET').toUpperCase();
  // Normalise body to a plain string regardless of URLSearchParams vs string
  const bodyStr = opts&&opts.body
    ? (opts.body instanceof URLSearchParams ? opts.body.toString() : String(opts.body))
    : '';
  let body = {};
  try{ body=Object.fromEntries(new URLSearchParams(bodyStr)); }catch(_){}

  // --- System status
  if(url==='/api/status') return simOkResp({
    uptime:    Math.round(simUptime),
    freeHeap:  Math.round(simHeap*1024),
    nodeCount: simState.length,
    maxNodes:  8,
    version:   'Simulation Edition',
    timeSet:   true,
    time:      (typeof gwTime!=='undefined'&&gwTime)||'00:00:00',
    ntpSynced: true,
    wifiMode:  'sta',
    ssid:      simSSID,
    wifiRSSI:  simRSSI,
    ip:        '192.168.1.42',
  });

  // --- Feature flags
  if(url==='/api/features')
    return simOkResp({encryption:true, knn:true});

  // --- Auth
  if(url==='/api/authstatus') return simOkResp({required:false, authenticated:true});
  if(url==='/api/login')      return simOkResp({ok:true, token:'sim-token'});
  if(url==='/api/logout')     return simOkResp({ok:true});

  // --- Node data (WS-disconnect fallback paths used by app.js)
  if(url==='/api/nodes') return simOkResp({nodes:simState.map(nodeSnapshot)});
  const liveM=url.match(/^\/api\/node\/(\d+)\/live$/);
  if(liveM){
    const s=simState.find(x=>x.id===parseInt(liveM[1]));
    return s ? simOkResp(nodeSnapshot(s)) : simOkResp({error:'not found'});
  }
  const histM=url.match(/^\/api\/node\/(\d+)\/history$/);
  if(histM){
    const s=simState.find(x=>x.id===parseInt(histM[1]));
    if(!s)return simOkResp([]);
    return simOkResp(s._hist.p.map((p,idx)=>({power:p,voltage:s._hist.v[idx],current:s._hist.i[idx]})));
  }

  // --- WiFi management
  if(url==='/api/info')
    return simOkResp({apSsid:'PowerTelemetry-Setup'});
  if(url==='/api/scan') return simOkResp({scanning:false, networks:[
    {ssid:'HomeNetwork_5G',  rssi:-42, secure:true},
    {ssid:'PLDT_FIBER_001A', rssi:-61, secure:true},
    {ssid:'AndroidAP',       rssi:-73, secure:true},
    {ssid:'GuestWifi',       rssi:-55, secure:false},
    {ssid:'DIRECT-Printer',  rssi:-80, secure:false},
  ]});
  if(url==='/api/connect'&&method==='POST') return simOkResp({ok:true});
  if(url==='/api/wifistatus') return simOkResp({
    apActive:false, connecting:false, connected:true,
    ip:'192.168.1.42', ssid:simSSID, rssi:simRSSI, ntpSynced:true,
  });
  if(url==='/api/disconnect')    return simOkResp({ok:true});
  if(url==='/api/forget')        return simOkResp({ok:true});
  if(url==='/api/staticip'&&method==='GET')  return simOkResp({enabled:false,ip:'',gateway:'',subnet:'',dns:''});
  if(url==='/api/staticip'&&method==='POST') return simOkResp({ok:true});
  if(url==='/api/staticip/clear')            return simOkResp({ok:true});

  // --- AP settings
  if(url==='/api/ap'&&method==='GET')  return simOkResp({ssid:'PowerTelemetry-Setup', hasPassword:false});
  if(url==='/api/ap'&&method==='POST') return simOkResp({ok:true});

  // --- Dashboard security (always open -- sim never needs a password)
  if(url==='/api/dashsecure'&&method==='GET')  return simOkResp({hasPassword:false});
  if(url==='/api/dashsecure'&&method==='POST') return simOkResp({ok:true});

  // --- Reboot (execReboot override handles this but simFetch is the safety net)
  if(url==='/api/reboot') return simOkResp({ok:true});

  // --- Encryption / RFID provisioning
  if(url==='/api/keygen'&&method==='POST'){
    const hex=[...Array(8)].map(()=>Math.floor(Math.random()*256).toString(16).padStart(2,'0').toUpperCase()).join('');
    appendLog('[CRYPTO] New AES-128 key generated: '+hex.slice(0,6)+'...');
    appendLog('[CRYPTO] Key stored in NVS (lora-net/aeskey)');
    return simOkResp({ok:true});
  }
  if(url==='/api/provision'&&method==='POST'){
    simProvisionStarted=Date.now();
    appendLog('[RFID] PN532 polling for card (5 s window)...');
    return simOkResp({ok:true});
  }
  if(url==='/api/provision/status'){
    if(!simProvisionStarted) return simOkResp({status:'idle'});
    const elapsed=(Date.now()-simProvisionStarted)/1000;
    if(elapsed<1.8) return simOkResp({status:'waiting'});
    if(elapsed<6){
      simProvisionStarted=null;
      appendLog('[RFID] Card detected (MIFARE Classic, UID=A3:2F:11:7C)');
      return simOkResp({status:'ok'});
    }
    simProvisionStarted=null;
    appendLog('[RFID] No card detected (5 s timeout)');
    return simOkResp({status:'timeout'});
  }

  // --- KNN model status
  if(url==='/api/knn/labels')
    return simOkResp({labels:SIM_PROFILES.filter(p=>!p._permanentlyOffline).map(p=>p.label)});
  if(url==='/api/knn/status') return simOkResp({
    loaded:       simKnnLoaded,
    profileCount: simKnnProfileCount,
    labelCount:   simKnnProfileCount,
    fromEmbed:    simKnnFromEmbed,
  });

  // --- KNN training
  if(url==='/api/knn/train/start'&&method==='POST'){
    const nodeId=parseInt(body.nodeId||'0');
    const label =(body.label||'').trim();
    const dur   =parseInt(body.durationMin||'5');
    if(!nodeId) return simOkResp({error:'No node selected'});
    if(!label)  return simOkResp({error:'Enter a label'});
    simKnnSessions[nodeId]={nodeId,label,durationSec:dur*60,startedAt:Date.now(),samples:0,active:true,finalized:false,slotIdx:nodeId-1};
    appendLog('[KNN] Training started: label="'+label+'" node='+nodeId+' dur='+dur+'m');
    return simOkResp({ok:true});
  }
  if(url==='/api/knn/train/stop'&&method==='POST'){
    const nodeId=parseInt(body.nodeId||'0');
    const sess=simKnnSessions[nodeId];
    if(sess&&sess.active){
      sess.active=false;
      if(sess.samples>=5){
        sess.finalized=true; simKnnProfileCount++;
        appendLog('[KNN] Session finalized: "'+sess.label+'" ('+sess.samples+' smp)');
      }else{
        appendLog('[KNN] Session discarded (too few samples: '+sess.samples+')');
      }
    }
    return simOkResp({ok:true});
  }
  if(url==='/api/knn/train/status'){
    const now=Date.now();
    const sessions=Object.values(simKnnSessions).map(sess=>{
      if(sess.active){
        const elapsed=(now-sess.startedAt)/1000;
        sess.samples=Math.floor(elapsed/3)*3;
        if(elapsed>=sess.durationSec){
          sess.active=false; sess.finalized=true; simKnnProfileCount++;
          appendLog('[KNN] Training complete: "'+sess.label+'" ('+sess.samples+' smp saved)');
        }
      }
      const rem=Math.max(0,Math.round(sess.durationSec-(Date.now()-sess.startedAt)/1000));
      return {slotIdx:sess.slotIdx, label:sess.label, active:sess.active, finalized:sess.finalized, samples:sess.samples, remainSec:rem};
    });
    return simOkResp({sessions});
  }

  // --- KNN model management
  if(url==='/api/knn/export'){
    const profiles=SIM_PROFILES.filter(p=>!p._permanentlyOffline).map(p=>({
      label:   p.label,
      samples: [{v:p.voltNominal, i:parseFloat((p.nominalPower/(p.voltNominal*p.pf)).toFixed(3)), p:p.nominalPower, pf:p.pf}],
    }));
    const blob=new Blob([JSON.stringify({version:1,profiles},null,2)],{type:'application/json'});
    return Promise.resolve({ok:true, status:200, blob:()=>Promise.resolve(blob)});
  }
  if(url==='/api/knn/import'&&method==='POST'){
    let count=3;
    try{ count=Math.max(1,(JSON.parse(bodyStr).profiles||[]).length); }catch(_){}
    simKnnLoaded=true; simKnnFromEmbed=false; simKnnProfileCount=count;
    appendLog('[KNN] Model imported: '+count+' profiles loaded');
    return simOkResp({ok:true, profileCount:count});
  }
  if(url==='/api/knn/profiles'&&method==='DELETE'){
    simKnnLoaded=false; simKnnProfileCount=0;
    Object.keys(simKnnSessions).forEach(k=>delete simKnnSessions[k]);
    appendLog('[KNN] Model erased from FRAM');
    return simOkResp({ok:true});
  }

  // Unmapped endpoint -- warn and return a generic ok so the caller keeps running
  console.warn('[SIM] Unmapped endpoint:', method, url);
  return simOkResp({ok:true});
}

// Replace window.fetch for all /api/ paths; pass everything else to the real fetch
(function(){
  const _orig=window.fetch;
  window.fetch=function(url,opts){
    if(typeof url==='string'&&url.startsWith('/api/'))return simFetch(url,opts||{});
    return _orig.call(window,url,opts);
  };
})();

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
      const s=simState.find(x=>x.id===cmd.node); if(s){s.energy=0;clearEnergyAnchor(cmd.node);} break;
    }
    case 'clear_all_energy':{ simState.forEach(s=>s.energy=0);Object.keys(energyAnchor).forEach(clearEnergyAnchor); break; }
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

// Reads directly from sim's own history ring-buffers; bypasses JSON round-trip
fetchHistory = async function(id){
  const s=simState.find(x=>x.id===id); if(!s)return;
  sparkData.power=[...s._hist.p];
  sparkData.voltage=[...s._hist.v];
  sparkData.current=[...s._hist.i];
  updateSparklines();
  if(!chart)return;
  const mc=MC[cMet]; const buf=s._hist[mc.k]||[];
  if(!buf.length)return;
  chart.data.labels=buf.map((_,i)=>{ const sec=(buf.length-1-i)*(SIM_TICK_MS/1000); return sec>60?Math.floor(sec/60)+'m':sec+'s'; });
  chart.data.datasets[0].data=[...buf]; chart.update('none');
};

// Prevents the post-reboot location.reload() loop from triggering in sim
execReboot = async function(){
  hideConfirm('cfmReboot');
  sT('gwUptime','Rebooting...');
  simUptime=0;
  await new Promise(r=>setTimeout(r,1500));
  sT('gwUptime',fU(0));
};

// Prevents the login overlay from appearing after logout in sim
doLogout = async function(){
  authToken=null;
  localStorage.removeItem('pt-token');
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
        {ssid:'AndroidAP',       rssi:-73, secure:true},
        {ssid:'GuestWifi',       rssi:-55, secure:false},
        {ssid:'DIRECT-Printer',  rssi:-80, secure:false},
      ]);
    }finally{ btn.classList.remove('spinning'); btn.disabled=false; }
  },900);
};

// Simulates the multi-step connect flow with progress states
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

// Prevents the real wifiDoForget from calling location.reload()
wifiDoForget = function(){
  hideConfirm('cfmForget');
  simSSID='PowerTelemetry-Setup'; simRSSI=0;
  wifiSelectedSSID='';
  const nl=$('wifiNetList');
  if(nl)nl.innerHTML='<div style="text-align:center;padding:18px 0;color:var(--txd);font-size:11px;font-family:\'Fira Code\',monospace">Credentials cleared. Scan to reconnect.</div>';
  const cs=$('wifiCredSection');if(cs)cs.style.display='none';
  $('wifiManualCb').checked=false;
  wifiToggleManual();
  setTimeout(fetchSys,500);
};

doSyncTime = function(){
  gwTimeSet=true;
  const btn=$('syncBtn'); if(!btn)return;
  btn.classList.add('ac-active');
  const prev=btn.innerHTML;
  btn.innerHTML='<svg width="12" height="12" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round"><polyline points="20 6 9 17 4 12"/></svg> Synced';
  clearTimeout(btn._t); btn._t=setTimeout(()=>{ btn.innerHTML=prev; btn.classList.remove('ac-active'); },2500);
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

// Bypass the auth flow so startApp() runs immediately without a real gateway
init = function(){
  document.addEventListener('DOMContentLoaded',()=>{
    // Clear stale localStorage anchors so random initial energies don't produce
    // blown-up rates against an anchor from a previous simulation session.
    simState.forEach(s=>clearEnergyAnchor(s.id));
    applyTheme(getTheme());
    $('costRateIn').value=costRate;
    $('btnClearLog')?.addEventListener('click',()=>{ const b=$('serialLogBox'); if(b)b.innerHTML=''; });
    buildLogFilterRow();
    hideLoginOverlay();
    injectSimBanner();
    startApp();
  });
};
