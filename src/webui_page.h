// ─────────────────────────────────────────────────────────────────────────────
// Embedded dashboard page. Vanilla HTML/CSS/JS, no external resources, served
// straight from flash (PROGMEM) at "/". Kept in its own header to keep
// webui.cpp readable.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once
#include <Arduino.h>

static const char PAGE_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1">
<title>CANBRIDGE</title>
<style>
:root{--bg:#0b0e12;--panel:#151a21;--panel2:#1c232c;--fg:#e6edf3;--dim:#8b96a3;
--acc:#3fb0ff;--good:#2ecc71;--warn:#f5a623;--bad:#ff4d4f;--border:#232b35}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--fg);font:14px/1.4 -apple-system,Segoe UI,Roboto,sans-serif;
padding:8px;padding-bottom:40px}
h1{font-size:16px;margin:4px 0 10px}
h2{font-size:12px;text-transform:uppercase;letter-spacing:.08em;color:var(--dim);margin:16px 0 6px}
.statusbar{display:flex;flex-wrap:wrap;gap:8px;background:var(--panel);border:1px solid var(--border);
border-radius:10px;padding:8px 10px;margin-bottom:8px}
.pill{background:var(--panel2);border-radius:20px;padding:4px 10px;font-size:12px;display:flex;gap:6px;align-items:center}
.grp{font-size:11px;font-weight:bold;letter-spacing:1px;color:var(--dim);align-self:center;min-width:52px}
.pill select,.pill button{background:var(--panel);color:inherit;border:1px solid var(--border);border-radius:6px;font-size:12px;padding:2px 6px}
.dot{width:8px;height:8px;border-radius:50%;background:var(--dim)}
.dot.on{background:var(--good)}
.dot.off{background:var(--bad)}
.grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(120px,1fr));gap:8px}
.tile{background:var(--panel);border:1px solid var(--border);border-radius:10px;padding:8px 10px}
.tile .lbl{color:var(--dim);font-size:11px;text-transform:uppercase;letter-spacing:.05em}
.tile .val{font-size:20px;font-weight:600;margin-top:2px}
.tile .val.big{font-size:30px}
.tile .sub{font-size:11px;color:var(--dim)}
.pos{color:var(--good)}.neg{color:var(--warn)}.bad{color:var(--bad)}
canvas.spark{width:100%;height:60px;background:var(--panel2);border-radius:8px;display:block}
.chartrow{display:grid;grid-template-columns:repeat(auto-fit,minmax(220px,1fr));gap:8px}
.chartbox{background:var(--panel);border:1px solid var(--border);border-radius:10px;padding:8px}
.chartbox .lbl{color:var(--dim);font-size:11px;text-transform:uppercase;margin-bottom:4px}
table{width:100%;border-collapse:collapse;font-size:12px}
th,td{padding:4px 6px;text-align:left;border-bottom:1px solid var(--border);white-space:nowrap}
th{color:var(--dim);font-weight:500;cursor:pointer;user-select:none}
.tablewrap{overflow-x:auto;background:var(--panel);border:1px solid var(--border);border-radius:10px;padding:4px}
.tx{background:var(--panel);border:1px solid var(--border);border-radius:10px;padding:10px}
.warnbox{background:#3a1a1a;border:1px solid var(--bad);border-radius:8px;padding:8px 10px;font-size:12px;margin-bottom:8px}
.row{display:flex;gap:8px;flex-wrap:wrap;align-items:center;margin-bottom:8px}
select,input[type=text]{background:var(--panel2);color:var(--fg);border:1px solid var(--border);
border-radius:6px;padding:6px 8px;font-size:13px}
input[type=text]{flex:1;min-width:160px;font-family:ui-monospace,monospace}
button{background:var(--acc);color:#08131c;border:none;border-radius:6px;padding:8px 14px;font-weight:600;
font-size:13px;cursor:pointer}
button:disabled{background:#324150;color:var(--dim);cursor:not-allowed}
.toggle{display:flex;align-items:center;gap:8px}
.switch{width:42px;height:24px;background:var(--panel2);border-radius:20px;position:relative;cursor:pointer;
border:1px solid var(--border)}
.switch .knob{width:18px;height:18px;background:var(--dim);border-radius:50%;position:absolute;top:2px;left:2px;
transition:.15s}
.switch.armed{background:#4a1414}
.switch.armed .knob{left:20px;background:var(--bad)}
.log{max-height:160px;overflow-y:auto;font-family:ui-monospace,monospace;font-size:11px;background:var(--panel2);
border-radius:8px;padding:6px 8px;margin-top:8px}
.log div{padding:1px 0}
.log .err{color:var(--bad)}
.log .ack{color:var(--good)}
canvas.cells{width:100%;height:100px;background:var(--panel2);border-radius:8px;display:block;cursor:crosshair}
.badge{background:#3a1a1a;border:1px solid var(--bad);border-radius:12px;padding:2px 8px;font-size:11px;
margin-left:6px;display:inline-block}
footer{color:var(--dim);font-size:11px;text-align:center;margin-top:16px}
details.settings{background:var(--panel);border:1px solid var(--border);border-radius:10px;padding:8px 10px;margin-bottom:8px}
details.settings summary{cursor:pointer;font-weight:600;list-style:none}
details.settings summary::-webkit-details-marker{display:none}
details.settings summary:before{content:'\25B8\00a0';display:inline-block;transition:.15s}
details.settings[open] summary:before{content:'\25BE\00a0'}
.settings-body{margin-top:10px}
.lockbanner{background:#3a1a1a;border:1px solid var(--bad);border-radius:8px;padding:8px 10px;font-size:12px;
margin-bottom:10px;display:none}
.lockbanner.show{display:block}
.field{margin-bottom:10px}
.field label{display:block;font-size:11px;color:var(--dim);text-transform:uppercase;letter-spacing:.05em;margin-bottom:4px}
.hint{font-size:11px;color:var(--dim);margin-top:4px}
.statusline{font-size:12px;margin-top:6px;min-height:16px}
.statusline.ok{color:var(--good)}
.statusline.err{color:var(--bad)}
input[readonly]{opacity:.8}
</style></head>
<body>
<h1>CANBRIDGE dashboard</h1>

<div class="statusbar">
  <span class="grp">BRIDGE</span>
  <span class="pill" title="Connection between this page and the bridge over WiFi">Link: <span class="dot" id="dotWs"></span><b id="wsState">connecting</b></span>
  <span class="pill" title="Which vehicle translation the bridge runs. Change in Settings below (applies after reboot)">Profile: <b id="profActive">-</b></span>
  <span class="pill" title="Green = CAN frames are arriving at the bridge right now">CAN traffic: <span class="dot" id="dotVeh"></span><b id="canLive">none</b></span>
  <span class="pill" title="Firmware version this board is running">FW: <b id="fw">-</b></span>
  <span class="pill" title="Time since the bridge last booted">Up: <span id="uptime">0s</span></span>
  <span class="pill" title="Bridge free RAM">Mem: <span id="heap">-</span></span>
</div>
<div class="statusbar">
  <span class="grp">CAR</span>
  <span class="pill" title="What the car is doing right now, from live CAN traffic; '-' when no data">State: <b id="state">-</b></span>
  <span class="pill" title="Car generation and battery size proven by CAN traffic; '-' until confirmed">Detected: <b id="detected">-</b></span>
</div>

<h2>Battery <span class="sub" id="battBusNote" title="Which of the bridge's two CAN connectors the battery answered on (wiring check)"></span></h2>
<div class="grid">
  <div class="tile"><div class="lbl">SOC</div><div class="val big" id="soc">--</div><div class="sub" id="socRaw"></div></div>
  <div class="tile"><div class="lbl">GIDs</div><div class="val" id="gids">--</div><div class="sub" id="kwh"></div></div>
  <div class="tile"><div class="lbl">SOH</div><div class="val" id="soh">--</div></div>
  <div class="tile"><div class="lbl">Pack voltage</div><div class="val" id="pv">--</div></div>
  <div class="tile"><div class="lbl">Pack current</div><div class="val" id="pi">--</div></div>
  <div class="tile"><div class="lbl">Power</div><div class="val" id="pw">--</div></div>
  <div class="tile"><div class="lbl">Temp (max/avg/min)</div><div class="val" id="temp">--</div></div>
  <div class="tile"><div class="lbl">Discharge limit</div><div class="val" id="dlim">--</div></div>
  <div class="tile"><div class="lbl">Charge limit</div><div class="val" id="clim">--</div></div>
  <div class="tile"><div class="lbl">Relay / Failsafe</div><div class="val" id="relay">--</div></div>
  <div class="tile"><div class="lbl">Battery DTC</div><div class="val" id="dtc">--</div></div>
</div>

<h2>Drive</h2>
<div class="grid">
  <div class="tile"><div class="lbl">Speed (approx)</div><div class="val" id="speed">--</div></div>
  <div class="tile"><div class="lbl">Gear</div><div class="val" id="gear">--</div></div>
  <div class="tile"><div class="lbl">ECO</div><div class="val" id="eco">--</div></div>
  <div class="tile"><div class="lbl">Torque</div><div class="val" id="torque">--</div></div>
  <div class="tile"><div class="lbl">Inverter voltage</div><div class="val" id="invv">--</div></div>
</div>

<h2>Battery cells</h2>
<div id="cellsPanel">
  <div class="chartbox" style="margin-bottom:8px">
    <div class="lbl">Cell voltages (mV) &mdash; red=min, green=max, orange dot=balancing</div>
    <canvas class="cells" id="chCells"></canvas>
    <div class="sub" id="cellTip">&nbsp;</div>
  </div>
  <div class="grid">
    <div class="tile"><div class="lbl">Min cell</div><div class="val" id="cellMin">--</div><div class="sub" id="cellMinN"></div></div>
    <div class="tile"><div class="lbl">Max cell</div><div class="val" id="cellMax">--</div><div class="sub" id="cellMaxN"></div></div>
    <div class="tile"><div class="lbl">Avg cell</div><div class="val" id="cellAvg">--</div></div>
    <div class="tile"><div class="lbl">Spread (imbalance)</div><div class="val big" id="cellSpread">--</div></div>
  </div>
  <div class="grid">
    <div class="tile"><div class="lbl">Hx</div><div class="val" id="hx">--</div></div>
    <div class="tile"><div class="lbl">Insulation</div><div class="val" id="insul">--</div></div>
    <div class="tile"><div class="lbl">T1</div><div class="val" id="t1">--</div></div>
    <div class="tile"><div class="lbl">T2</div><div class="val" id="t2">--</div></div>
    <div class="tile"><div class="lbl">T3</div><div class="val" id="t3">--</div></div>
    <div class="tile"><div class="lbl">T4</div><div class="val" id="t4">--</div></div>
  </div>
  <div class="sub" id="cellIds">Part -- &middot; Serial -- &middot; BMS ID --</div>
  <div class="sub" id="cellStatus">waiting for battery diagnostics&hellip;</div>
</div>

<h2>Trends (~60s)</h2>
<div class="chartrow">
  <div class="chartbox"><div class="lbl">Power (kW)</div><canvas class="spark" id="chPower"></canvas></div>
  <div class="chartbox"><div class="lbl">SOC (%)</div><canvas class="spark" id="chSoc"></canvas></div>
  <div class="chartbox"><div class="lbl">Temp (&deg;C)</div><canvas class="spark" id="chTemp"></canvas></div>
</div>

<h2>Frame monitor</h2>
<div class="chartrow">
  <div class="tablewrap"><b>Bus A</b>
    <table id="tblA"><thead><tr><th data-k="id">ID</th><th data-k="hz">Hz</th><th data-k="count">Count</th><th>Data</th></tr></thead><tbody></tbody></table>
  </div>
  <div class="tablewrap"><b>Bus B</b>
    <table id="tblB"><thead><tr><th data-k="id">ID</th><th data-k="hz">Hz</th><th data-k="count">Count</th><th>Data</th></tr></thead><tbody></tbody></table>
  </div>
</div>

<details class="settings">
  <summary>&#9881; Settings</summary>
  <div class="settings-body">
    <div class="lockbanner" id="lockBanner">&#128274; Controls locked &mdash; connect on the bench or park the car (P, stationary) to change settings or transmit.</div>

    <div class="field">
      <label>Vehicle profile</label>
      <div class="row" style="margin-bottom:0">
        <select id="profSel"><option value="leaf">LEAF</option><option value="env200">e-NV200</option><option value="monitor">Monitor</option></select>
        <button id="profReboot" style="display:none">reboot to apply</button>
      </div>
      <div class="hint">Applies after reboot. LEAF vs e-NV200 cannot be auto-detected — set it here. Monitor is pure pass-through for an unmodified car.</div>
      <div class="statusline" id="profStatus"></div>
    </div>

    <div class="field">
      <label>AP password</label>
      <div class="row" style="margin-bottom:0">
        <input type="password" id="apPw" placeholder="new password" autocomplete="new-password">
        <button id="apPwSave">Save password</button>
      </div>
      <div class="hint">8&ndash;63 characters, applied after reboot. Current password is never shown here.</div>
      <div class="statusline" id="apPwStatus"></div>
    </div>

    <div class="field">
      <label>Reboot</label>
      <div class="row" style="margin-bottom:0"><button id="rebootBtn">Reboot bridge</button></div>
      <div class="statusline" id="rebootStatus"></div>
    </div>

    <h2 style="margin-top:18px">Custom CAN transmit</h2>
    <div class="tx">
      <div class="warnbox">&#9888; Transmitting on a live vehicle bus can trigger faults or unsafe behavior.
      Only use when you understand the frame.</div>
      <div class="row">
        <div class="toggle"><div class="switch" id="armSwitch"><div class="knob"></div></div>
        <span id="armLabel">Arm transmit: OFF</span></div>
      </div>
      <div class="row">
        <select id="txBus">
          <option value="battery">Battery (auto)</option>
          <option value="vehicle">Vehicle (auto)</option>
          <option value="A">Raw bus A</option>
          <option value="B">Raw bus B</option>
        </select>
        <input type="text" id="txId" placeholder="ID hex e.g. 79B">
        <input type="text" id="txData" placeholder="Data hex e.g. 02 21 01 00 00 00 00 00">
        <span id="txDlc" class="sub">DLC: 0</span>
        <button id="txSend" disabled>Send</button>
      </div>
      <div class="log" id="txLog"></div>
    </div>
  </div>
</details>

<footer>CANBRIDGE &middot; AP 10.0.0.1 &middot; refreshes every 250ms</footer>

<script>
var ws, wsUp=false;
var hist={p:[],s:[],t:[]};
var HIST_N=60;

function $(id){return document.getElementById(id);}
function fmt(v,d){return (v===undefined||v===null||isNaN(v))?'--':Number(v).toFixed(d===undefined?1:d);}

function connect(){
  var proto = location.protocol==='https:'?'wss:':'ws:';
  ws = new WebSocket(proto+'//'+location.host+'/ws');
  ws.onopen=function(){wsUp=true;$('wsState').textContent='connected';$('dotWs').className='dot on';};
  ws.onclose=function(){wsUp=false;$('wsState').textContent='disconnected';$('dotWs').className='dot off';setTimeout(connect,1500);};
  ws.onerror=function(){ws.close();};
  ws.onmessage=function(ev){
    var d;
    try{d=JSON.parse(ev.data);}catch(e){return;}
    if(d.type==='ack'||d.type==='err'){logTx(d);statusReply(d);return;}
    if(d.type==='cells'){renderCells(d);return;}
    render(d);
  };
}

function logTx(d){
  var el=document.createElement('div');
  el.className=d.type;
  el.textContent='['+new Date().toLocaleTimeString()+'] '+d.msg;
  var log=$('txLog');
  log.insertBefore(el,log.firstChild);
  while(log.children.length>50) log.removeChild(log.lastChild);
}

// Routes the ack/err for the most recently issued settings command to its
// own status line (the WS reply has no cmd tag, so we track what we last sent).
var lastSettingsCmd=null;
function statusReply(d){
  if(!lastSettingsCmd)return;
  var el=$(lastSettingsCmd+'Status');
  lastSettingsCmd=null;
  if(!el)return;
  el.textContent=d.msg;
  el.className='statusline '+(d.type==='ack'?'ok':'err');
}

var lastFrameTs=0;
var writeSafe=true;
function render(d){
  lastFrameTs=Date.now();
  syncProfile(d);
  writeSafe=!!d.write_safe;
  applyLock();
  $('detected').textContent=d.detected||'-';
  var canLive = d.can_rx_age_ms>=0 && d.can_rx_age_ms<2000;
  $('dotVeh').className='dot '+(canLive?'on':'off');
  $('canLive').textContent=canLive?'live':'none';
  var states=['Idle','Driving','Charging'];
  var stateTxt='-';
  if(canLive){
    stateTxt=states[d.car_state]||'Idle';
    if(d.vcm_awake===0) stateTxt='Asleep';
  }
  $('state').textContent=stateTxt;
  $('battBusNote').textContent = d.battery_bus<0?'':('— wired to bridge port '+(d.battery_bus===0?'A':'B'));
  $('fw').textContent=d.fw||'-';
  $('uptime').textContent=Math.floor(d.uptime_ms/1000)+'s';
  $('heap').textContent=(d.free_heap/1024).toFixed(1)+'KB';

  var soc = d.usable_soc_pct>=0 ? d.usable_soc_pct : (d.soc_tenth_pct/10);
  $('soc').textContent=fmt(soc,1)+'%';
  $('socRaw').textContent='0.1% raw: '+fmt(d.soc_tenth_pct/10,1)+'%';
  $('gids').textContent=d.gids>=0?d.gids:'--';
  $('kwh').textContent=d.gids>=0?(d.gids*80/1000).toFixed(2)+' kWh':'';
  $('soh').textContent=d.soh_pct+'%';
  $('pv').textContent=fmt(d.pack_voltage_v,1)+' V';
  $('pi').textContent=fmt(d.pack_current_a,1)+' A';
  var pw=$('pw'); pw.textContent=fmt(d.pack_power_kw,2)+' kW';
  pw.className='val '+(d.pack_power_kw<0?'pos':(d.pack_power_kw>0?'neg':''));
  $('temp').textContent=d.temp_max_c+' / '+d.temp_avg_c+' / '+d.temp_min_c+' °C';
  $('dlim').textContent=fmt(d.max_discharge_kw,2)+' kW';
  $('clim').textContent=fmt(d.max_charge_kw,2)+' kW';
  $('relay').textContent='FS:'+d.lb_failsafe_status+' Cut:'+d.lb_relay_cut_request+' On:'+d.lb_main_relay_on;
  var dtcEl=$('dtc'); dtcEl.textContent=d.battery_dtc?('0x'+d.battery_dtc.toString(16)):'none';
  dtcEl.className='val '+(d.battery_dtc?'bad':'');

  $('speed').textContent=fmt(d.speed_kmh,1)+' km/h';
  var g=['P','?','R','N','D/B'];
  $('gear').textContent=g[d.gear]!==undefined?g[d.gear]:d.gear;
  $('eco').textContent=d.eco_on?'ON':'OFF';
  $('torque').textContent=fmt(d.torque_nm,1)+' Nm';
  $('invv').textContent=fmt(d.inverter_voltage_v,0)+' V';

  push(hist.p,d.pack_power_kw); push(hist.s,soc); push(hist.t,d.temp_avg_c);
  draw('chPower',hist.p); draw('chSoc',hist.s); draw('chTemp',hist.t);

  renderTable('tblA', d.frames.filter(function(f){return f.bus==='A';}));
  renderTable('tblB', d.frames.filter(function(f){return f.bus==='B';}));

  $('txSend').disabled = !armed || !writeSafe;
}

function applyLock(){
  $('lockBanner').className='lockbanner'+(writeSafe?'':' show');
  var ids=['profSel','profReboot','apPw','apPwSave','rebootBtn','armSwitch'];
  ids.forEach(function(id){
    var el=$(id);
    if(!el)return;
    if(el.id==='armSwitch'){
      el.style.pointerEvents=writeSafe?'':'none';
      el.style.opacity=writeSafe?'':'.5';
    } else {
      el.disabled=!writeSafe;
    }
  });
  if(!writeSafe && armed){
    armed=false;
    $('armSwitch').className='switch';
    $('armLabel').textContent='Arm transmit: OFF';
  }
  $('txSend').disabled = !armed || !writeSafe;
}

function push(arr,v){arr.push(v);while(arr.length>HIST_N)arr.shift();}

function draw(id,arr){
  var c=$(id); var w=c.clientWidth||220,h=60;
  if(c.width!==w*2){c.width=w*2;c.height=h*2;}
  var ctx=c.getContext('2d'); ctx.setTransform(2,0,0,2,0,0);
  ctx.clearRect(0,0,w,h);
  if(arr.length<2)return;
  var min=Math.min.apply(null,arr),max=Math.max.apply(null,arr);
  if(min===max){min-=1;max+=1;}
  ctx.strokeStyle='#3fb0ff';ctx.lineWidth=1.5;ctx.beginPath();
  arr.forEach(function(v,i){
    var x=i/(HIST_N-1)*w;
    var y=h-((v-min)/(max-min))*h;
    if(i===0)ctx.moveTo(x,y);else ctx.lineTo(x,y);
  });
  ctx.stroke();
}

var lastCells=null, hoverCell=-1;
function renderCells(d){
  lastCells=d;

  var mv=d.mv;
  var min=Math.min.apply(null,mv), max=Math.max.apply(null,mv);
  var minI=mv.indexOf(min), maxI=mv.indexOf(max);
  var avg=mv.reduce(function(a,b){return a+b;},0)/mv.length;
  $('cellMin').textContent=min+' mV'; $('cellMinN').textContent='cell '+minI;
  $('cellMax').textContent=max+' mV'; $('cellMaxN').textContent='cell '+maxI;
  $('cellAvg').textContent=avg.toFixed(1)+' mV';
  $('cellSpread').textContent=(max-min)+' mV';

  $('hx').textContent=fmt(d.hx,2)+'%';
  $('insul').textContent=d.insulation;
  var ts=d.temps||[null,null,null,null];
  ['t1','t2','t3','t4'].forEach(function(id,i){
    var v=ts[i];
    $(id).textContent=(v===null||v===undefined)?'—':v+' °C';
  });

  $('cellIds').textContent='Part '+(d.part||'--')+' · Serial '+(d.serial||'--')+' · BMS ID '+(d.bmsid||'--');
  var statusEl=$('cellStatus');
  statusEl.innerHTML='polled '+d.poll_age_s+'s ago'+
    (d.paused?' <span class="badge">paused: external OBD tool detected</span>':'');

  drawCells(d);
}

// shunts: 24 hex chars, 4 bits/char; cell n's bit lives at hex digit floor(n/4), bit (n%4).
function shuntActive(shuntsHex,n){
  var nib=parseInt(shuntsHex.charAt(Math.floor(n/4)),16)||0;
  return (nib>>(n%4))&1;
}

function drawCells(d){
  var c=$('chCells'); var w=c.clientWidth||300,h=100;
  if(c.width!==w*2||c.height!==h*2){c.width=w*2;c.height=h*2;}
  var ctx=c.getContext('2d'); ctx.setTransform(2,0,0,2,0,0);
  ctx.clearRect(0,0,w,h);
  var mv=d.mv;
  if(!mv||!mv.length)return;
  var min=Math.min.apply(null,mv),max=Math.max.apply(null,mv);
  var lo=min-10,hi=max+10; if(hi<=lo)hi=lo+1;
  var minI=mv.indexOf(min), maxI=mv.indexOf(max);
  var bw=w/mv.length;
  for(var i=0;i<mv.length;i++){
    var bh=Math.max(((mv[i]-lo)/(hi-lo))*h,1);
    var x=i*bw, y=h-bh;
    ctx.fillStyle = i===minI?'#ff4d4f':(i===maxI?'#2ecc71':(i===hoverCell?'#ffffff':'#3fb0ff'));
    ctx.fillRect(x+0.5,y,Math.max(bw-1,1),bh);
    if(d.shunts && shuntActive(d.shunts,i)){
      ctx.fillStyle='#f5a623';
      ctx.beginPath();
      ctx.arc(x+bw/2,h-2,1.5,0,2*Math.PI);
      ctx.fill();
    }
  }
}

function cellHoverAt(clientX){
  if(!lastCells)return;
  var c=$('chCells'); var rect=c.getBoundingClientRect();
  var idx=Math.floor((clientX-rect.left)/rect.width*lastCells.mv.length);
  if(idx<0)idx=0; if(idx>=lastCells.mv.length)idx=lastCells.mv.length-1;
  hoverCell=idx;
  $('cellTip').textContent='cell '+idx+': '+lastCells.mv[idx]+' mV';
  drawCells(lastCells);
}
// Placeholder skeleton before any data: 96 uniform dim bars so the full
// monitored layout is visible from the first page load.
function drawCellsPlaceholder(){
  var c=$('chCells'); var w=c.clientWidth||300,h=100;
  if(c.width!==w*2||c.height!==h*2){c.width=w*2;c.height=h*2;}
  var ctx=c.getContext('2d'); ctx.setTransform(2,0,0,2,0,0);
  ctx.clearRect(0,0,w,h);
  var bw=w/96;
  ctx.fillStyle='#2a3442';
  for(var i=0;i<96;i++) ctx.fillRect(i*bw+0.5,h*0.35,Math.max(bw-1,1),h*0.65);
}
drawCellsPlaceholder();

$('chCells').addEventListener('mousemove',function(ev){cellHoverAt(ev.clientX);});
$('chCells').addEventListener('mouseleave',function(){hoverCell=-1;if(lastCells)drawCells(lastCells);});
$('chCells').addEventListener('touchstart',function(ev){
  if(ev.touches&&ev.touches[0])cellHoverAt(ev.touches[0].clientX);
},{passive:true});

var sortKey={tblA:'id',tblB:'id'};
function renderTable(id,rows){
  rows.sort(function(a,b){
    var k=sortKey[id];
    if(k==='id')return a.id.localeCompare(b.id);
    return b[k]-a[k];
  });
  var tbody=$(id).querySelector('tbody');
  tbody.innerHTML='';
  rows.forEach(function(f){
    var tr=document.createElement('tr');
    tr.innerHTML='<td>'+f.id+'</td><td>'+f.hz.toFixed(1)+'</td><td>'+f.count+'</td><td>'+f.data+'</td>';
    tbody.appendChild(tr);
  });
}
document.querySelectorAll('th[data-k]').forEach(function(th){
  th.addEventListener('click',function(){
    var tbl=th.closest('table').id;
    sortKey[tbl]=th.getAttribute('data-k');
  });
});

var armed=false;
$('armSwitch').addEventListener('click',function(){
  if(!writeSafe)return;
  armed=!armed;
  $('armSwitch').className='switch'+(armed?' armed':'');
  $('armLabel').textContent='Arm transmit: '+(armed?'ON':'OFF');
  $('txSend').disabled=!armed||!writeSafe;
});

function updateDlc(){
  var bytes=$('txData').value.trim().split(/\s+/).filter(Boolean);
  $('txDlc').textContent='DLC: '+bytes.length;
}
$('txData').addEventListener('input',updateDlc);

$('txSend').addEventListener('click',function(){
  if(!armed||!wsUp)return;
  var idHex=$('txId').value.trim().replace(/^0x/i,'');
  var bytes=$('txData').value.trim().split(/\s+/).filter(Boolean).map(function(b){return parseInt(b,16)||0;});
  var msg={cmd:'tx',bus:$('txBus').value,id:'0x'+idHex,dlc:bytes.length,data:bytes};
  ws.send(JSON.stringify(msg));
  logTx({type:'ack',msg:'sent -> '+JSON.stringify(msg)});
});

setInterval(function(){
  if(!lastFrameTs)return;
  if(Date.now()-lastFrameTs>3000){$('dotVeh').className='dot off';$('canLive').textContent='none';$('state').textContent='-';}
},1000);

// Profile selector: sends {"cmd":"setprofile",...} over the WS; translation
// applies after reboot.
var profBusy=false;
function syncProfile(d){
  var sel=$('profSel');
  var map={'LEAF':'leaf','e-NV200':'env200','Monitor':'monitor'};
  if(!profBusy && document.activeElement!==sel) sel.value=map[d.profile_stored]||'monitor';
  $('profActive').textContent=d.vehicle||'-';
  $('profReboot').style.display=(d.profile_stored!==d.vehicle)?'':'none';
}
$('profSel').addEventListener('change',function(){
  if(!wsUp)return;
  profBusy=true;
  lastSettingsCmd='prof';
  ws.send(JSON.stringify({cmd:'setprofile',value:$('profSel').value}));
  setTimeout(function(){profBusy=false;},500);
});
$('profReboot').addEventListener('click',function(){
  if(!wsUp)return;
  lastSettingsCmd='reboot';
  ws.send(JSON.stringify({cmd:'reboot'}));
});
$('rebootBtn').addEventListener('click',function(){
  if(!wsUp)return;
  lastSettingsCmd='reboot';
  ws.send(JSON.stringify({cmd:'reboot'}));
});
$('apPwSave').addEventListener('click',function(){
  if(!wsUp)return;
  var pw=$('apPw').value;
  lastSettingsCmd='apPw';
  ws.send(JSON.stringify({cmd:'setappw',value:pw}));
  $('apPw').value='';
});

// Demo mode (?demo): synthetic data through the SAME render path, client-side
// only — the firmware never fabricates frames. For bench UI evaluation.
var DEMO=/[?#&]demo/.test(location.search+location.hash);
if(DEMO){
  var banner=document.createElement('div');
  banner.textContent='DEMO DATA — synthetic values, not a real battery';
  banner.style.cssText='position:sticky;top:0;z-index:99;background:#f5a623;color:#000;text-align:center;padding:6px;font-weight:bold';
  document.body.insertBefore(banner,document.body.firstChild);
  $('wsState').textContent='demo';$('dotWs').className='dot on';
  var t0=Date.now(), demoGen=0;
  var demoBase=[]; for(var i=0;i<96;i++)demoBase.push(3810+Math.round(Math.sin(i*1.7)*8));
  demoBase[37]-=55;  // one weak cell to make min/spread interesting
  var demoShunts='000400000000010000002000';
  function demoCells(){
    demoGen++;
    renderCells({type:'cells',gen:demoGen,
      mv:demoBase.map(function(m){return m+Math.round(Math.random()*4-2);}),
      shunts:demoShunts,temps:[22,23,null,21],hx:2.31,insulation:3021,
      part:'3NK2AR4',serial:'230UK1192E00148',bmsid:'DCA10055',poll_age_s:2,paused:0});
  }
  setInterval(function(){
    var t=(Date.now()-t0)/1000;
    var drive=Math.sin(t/8);
    var pkw=drive*40+Math.sin(t*1.3)*6;
    var soc=87-(t/60)%5;
    var v=360-drive*8;
    render({fw:'demo',vehicle:'LEAF',profile_stored:'LEAF',uptime_ms:t*1000,free_heap:220000,battery_bus:1,can_rx_age_ms:50,write_safe:1,
      detected:'AZE0 (2013-17) · 40 kWh',
      soc_tenth_pct:soc*10,usable_soc_pct:Math.round(soc),gids:Math.round(soc*4.9),soh_pct:91,
      pack_voltage_v:v,pack_current_a:pkw*1000/v,pack_power_kw:pkw,
      temp_max_c:24,temp_avg_c:22,temp_min_c:21,battery_dtc:0,
      max_discharge_kw:110,max_charge_kw:60,
      lb_failsafe_status:0,lb_relay_cut_request:0,lb_main_relay_on:1,
      torque_nm:drive*120,inverter_voltage_v:v-2,gear:4,eco_on:1,
      speed_kmh:Math.max(0,drive*90),charge_power_kw:0,target_soc_80:0,vcm_awake:1,car_state:1,
      frames:[
        {bus:'B',id:'0x1DB',count:Math.round(t*100),hz:100,data:'2F0A31C000000000',age_ms:5},
        {bus:'B',id:'0x55B',count:Math.round(t*10),hz:10,data:'6A80000000000000',age_ms:40},
        {bus:'B',id:'0x5BC',count:Math.round(t*10),hz:10,data:'4E60000000000000',age_ms:60},
        {bus:'A',id:'0x1D4',count:Math.round(t*100),hz:100,data:'F70700004000007F',age_ms:4},
        {bus:'A',id:'0x11A',count:Math.round(t*100),hz:100,data:'4E40000000000000',age_ms:8},
        {bus:'A',id:'0x284',count:Math.round(t*50),hz:50,data:'0000000012340000',age_ms:12}
      ]});
  },250);
  demoCells();
  setInterval(demoCells,3000);
}else{
  connect();
}
</script>
</body></html>
)rawliteral";
