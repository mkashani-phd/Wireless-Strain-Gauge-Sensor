// ---------------------------------------------------------------------------
// webpage.h  -  the whole PC-side GUI, served from flash by the ESP32.
// Works in any browser on Linux / macOS / Windows. Nothing to install.
// ---------------------------------------------------------------------------
#pragma once
#include <Arduino.h>

const char INDEX_HTML[] PROGMEM = R"PAGE(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Contact Pressure</title>
<style>
:root{
  --bg:#0e1116; --panel:#161b22; --line:#2a313c; --fg:#e6edf3;
  --dim:#8b949e; --acc:#2f81f7; --ok:#3fb950; --warn:#d29922; --bad:#f85149;
}
*{box-sizing:border-box}
body{margin:0;font:14px/1.45 ui-sans-serif,system-ui,-apple-system,Segoe UI,Roboto,sans-serif;
     background:var(--bg);color:var(--fg)}
header{display:flex;flex-wrap:wrap;gap:18px;align-items:center;padding:10px 16px;
       background:var(--panel);border-bottom:1px solid var(--line);position:sticky;top:0;z-index:5}
h1{font-size:15px;margin:0;font-weight:600;letter-spacing:.3px}
.chip{display:flex;align-items:center;gap:6px;font-size:12px;color:var(--dim)}
.dot{width:9px;height:9px;border-radius:50%;background:var(--bad)}
.dot.on{background:var(--ok)}
.batt{width:52px;height:13px;border:1px solid var(--dim);border-radius:3px;position:relative}
.batt::after{content:"";position:absolute;right:-4px;top:3px;width:3px;height:7px;background:var(--dim)}
.batt i{display:block;height:100%;background:var(--ok);width:0}
main{display:grid;grid-template-columns:1fr 1fr 380px;gap:14px;padding:14px;align-items:start}
@media(max-width:1200px){main{grid-template-columns:1fr 1fr}}
@media(max-width:900px){main{grid-template-columns:1fr}}
.card{background:var(--panel);border:1px solid var(--line);border-radius:10px;padding:14px}
.card h2{font-size:11px;text-transform:uppercase;letter-spacing:1px;color:var(--dim);
         margin:0 0 12px;font-weight:600}
.reading{font:700 48px/1 ui-monospace,SFMono-Regular,Menlo,monospace;
         text-align:center;padding:6px 0}
.reading small{font-size:18px;color:var(--dim);font-weight:500}
.sub{text-align:center;color:var(--dim);font-size:12px;font-family:ui-monospace,monospace}
canvas{width:100%;height:160px;display:block;background:#0b0e13;border-radius:6px;margin-top:10px}
.chanlabel{color:var(--acc);font-weight:600}
.row{display:flex;gap:8px;align-items:center;margin:8px 0;flex-wrap:wrap}
.row label{width:96px;color:var(--dim);font-size:12px;flex:none}
select,input,button{background:#0d1117;color:var(--fg);border:1px solid var(--line);
       border-radius:6px;padding:6px 9px;font:inherit;font-size:13px}
input{width:110px}
button{cursor:pointer;background:#21262d}
button:hover{border-color:var(--acc)}
button.pri{background:var(--acc);border-color:var(--acc);color:#fff;font-weight:600}
button.pri:hover{filter:brightness(1.15)}
table{width:100%;border-collapse:collapse;font-family:ui-monospace,monospace;font-size:12px}
th,td{text-align:left;padding:3px 5px;border-bottom:1px solid var(--line)}
th{color:var(--dim);font-weight:500}
td input{width:62px;padding:2px 5px;font-family:ui-monospace,monospace}
.bits{font-family:ui-monospace,monospace;font-size:11px;color:var(--dim);
      background:#0b0e13;border-radius:6px;padding:8px;margin-top:8px;white-space:pre-wrap}
#log{height:120px;overflow:auto;background:#0b0e13;border-radius:6px;padding:8px;
     font-family:ui-monospace,monospace;font-size:11px;color:var(--dim)}
.tabs{display:flex;gap:4px;margin-bottom:10px}
.tabs button{flex:1;font-size:12px}
.tabs button.act{background:var(--acc);border-color:var(--acc);color:#fff}
.hint{color:var(--dim);font-size:11px;margin:6px 0 0}
</style>
</head>
<body>

<header>
  <h1>Contact Pressure</h1>
  <span class="chip"><span class="dot" id="dot"></span><span id="conn">offline</span></span>
  <span class="chip">IP <b id="ip">-</b></span>
  <span class="chip">RSSI <b id="rssi">-</b></span>
  <span class="chip"><span class="batt"><i id="battbar"></i></span><b id="battxt">-</b></span>
  <span class="chip">rate <b id="sps">-</b></span>
  <span class="chip">up <b id="up">-</b></span>
</header>

<main>
  <!-- ================= channel columns (built by JS, one per NAU7802 input) ================= -->
  <div id="colCh0"></div>
  <div id="colCh1"></div>

  <!-- ================= right column ================= -->
  <div>
    <div class="card">
      <h2>Streaming</h2>
      <div class="row">
        <button id="bPause">Pause</button>
        <label style="width:auto">window</label>
        <select id="span">
          <option value="200">200 pts</option>
          <option value="600" selected>600 pts</option>
          <option value="2000">2000 pts</option>
        </select>
      </div>
      <div class="row">
        <button id="bRec">Record</button>
        <button id="bSave">Download CSV</button>
        <span class="hint" id="recinfo"></span>
      </div>
      <p class="hint">One recording covers both channels together, timestamped
      by this computer's clock, with battery voltage and the active settings
      saved in the file header.</p>
    </div>

    <div class="card" style="margin-top:14px">
      <h2>NAU7802 settings (chip-wide)</h2>
      <div class="row"><label>PGA gain</label>
        <select id="gain">
          <option value="0">x1</option><option value="1">x2</option>
          <option value="2">x4</option><option value="3">x8</option>
          <option value="4">x16</option><option value="5">x32</option>
          <option value="6">x64</option><option value="7">x128</option>
        </select></div>
      <div class="row"><label>Sample rate</label>
        <select id="spsSel">
          <option value="0">10 SPS</option><option value="1">20 SPS</option>
          <option value="2">40 SPS</option><option value="3">80 SPS</option>
          <option value="7">320 SPS</option>
        </select></div>
      <div class="row"><label>LDO (AVDD)</label>
        <select id="ldo">
          <option value="0">4.5 V</option><option value="1">4.2 V</option>
          <option value="2">3.9 V</option><option value="3">3.6 V</option>
          <option value="4">3.3 V</option><option value="5">3.0 V</option>
          <option value="6">2.7 V</option><option value="7">2.4 V</option>
        </select>
        <span class="hint">keep &lt;= 3.0 V on a 3.3 V Feather</span></div>
      <div class="row">
        <button id="bAfe">Internal AFE cal</button>
        <button id="bRst">Reset chip</button>
      </div>
      <p class="hint">Both channels are sampled continuously, round-robin. Gain /
      rate / LDO apply chip-wide and re-run offset calibration on both channels,
      as the datasheet requires.</p>
    </div>

    <div class="card" style="margin-top:14px">
      <h2>Register map</h2>
      <div class="tabs">
        <button id="tAll" class="act">All</button>
        <button id="tKey">Key only</button>
        <button id="bRefresh">Refresh</button>
      </div>
      <table id="regs"><thead><tr><th>Addr</th><th>Name</th><th>Hex</th><th>Bin</th><th>Write</th></tr></thead><tbody></tbody></table>
      <div class="bits" id="decode"></div>
    </div>

    <div class="card" style="margin-top:14px">
      <h2>Log</h2>
      <div id="log"></div>
    </div>
  </div>
</main>

<script>
// ---------------------------------------------------------------- constants
const REGNAME = {
  0x00:"PU_CTRL", 0x01:"CTRL1", 0x02:"CTRL2",
  0x03:"OCAL1_B2",0x04:"OCAL1_B1",0x05:"OCAL1_B0",
  0x06:"GCAL1_B3",0x07:"GCAL1_B2",0x08:"GCAL1_B1",0x09:"GCAL1_B0",
  0x0A:"OCAL2_B2",0x0B:"OCAL2_B1",0x0C:"OCAL2_B0",
  0x0D:"GCAL2_B3",0x0E:"GCAL2_B2",0x0F:"GCAL2_B1",0x10:"GCAL2_B0",
  0x11:"I2C_CTRL",0x12:"ADCO_B2",0x13:"ADCO_B1",0x14:"ADCO_B0",
  0x15:"ADC",0x16:"OTP_B1",0x17:"OTP_B0",
  0x1B:"PGA",0x1C:"PGA_PWR",0x1F:"DEVICE_REV"
};
const KEYREGS = [0x00,0x01,0x02,0x11,0x15,0x1B,0x1C,0x1F];
const GAINS = [1,2,4,8,16,32,64,128];
const SPSV  = {0:10,1:20,2:40,3:80,7:320};
const LDOV  = ["4.5","4.2","3.9","3.6","3.3","3.0","2.7","2.4"];
const NCHAN = 2;

// ---------------------------------------------------------------- per-channel column markup
function chanColumnHTML(ch){
  const n = ch + 1;
  return `
    <div class="card">
      <h2><span class="chanlabel">CH${n}</span> live reading</h2>
      <div class="reading"><span id="val${ch}">--.--</span><small id="unit${ch}"> g</small></div>
      <div class="sub">raw <span id="raw${ch}">0</span> counts &nbsp;|&nbsp; scale <span id="kshow${ch}">0</span> cnt/unit &nbsp;|&nbsp; offset <span id="oshow${ch}">0</span></div>
      <canvas id="chart${ch}"></canvas>
      <div class="row" style="margin-top:12px">
        <button class="pri" id="bTare${ch}">Tare (zero)</button>
        <button id="bClear${ch}">Clear chart</button>
      </div>
    </div>
    <div class="card" style="margin-top:14px">
      <h2>CH${n} calibration</h2>
      <div class="row">
        <label>Known mass</label>
        <input id="calw${ch}" type="number" step="any" value="100">
        <button id="bCal${ch}">Set scale</button>
      </div>
      <div class="row">
        <label>Scale factor</label>
        <input id="kset${ch}" type="number" step="any">
        <button id="bK${ch}">Apply</button>
      </div>
      <div class="row">
        <label>Offset</label>
        <input id="oset${ch}" type="number" step="1">
        <button id="bO${ch}">Apply</button>
      </div>
      <div class="row">
        <label>Unit label</label>
        <input id="uset${ch}" value="g" style="width:70px">
        <button id="bU${ch}">Apply</button>
      </div>
      <div class="row">
        <label>Averaging</label>
        <select id="avg${ch}">
          <option value="1">1 (raw)</option><option value="2">2</option>
          <option value="4">4</option><option value="8">8</option>
          <option value="16">16</option><option value="32">32</option>
          <option value="64">64</option>
        </select>
      </div>
    </div>`;
}
for (let ch = 0; ch < NCHAN; ch++) document.getElementById("colCh"+ch).innerHTML = chanColumnHTML(ch);

// ---------------------------------------------------------------- state
let ws, paused=false, keyOnly=false;
let regcache = {}, lastStatus = null;
const chanState = [];
for (let ch = 0; ch < NCHAN; ch++) chanState.push({
  offset:0, kscale:1, unit:"g", data:[], times:[], lastMs:null, maxPts:600
});

// one recording covers both channels, timestamped by this computer's clock
let recording = false, recRows = [];
let battV = 0, battPct = 0;
const lastKnown = [{raw:0, units:0}, {raw:0, units:0}];

const $ = id => document.getElementById(id);
const log = m => { const d=$("log");
  d.innerHTML += new Date().toLocaleTimeString()+"  "+m+"<br>";
  d.scrollTop = d.scrollHeight; };

// ---------------------------------------------------------------- websocket
function connect(){
  const host = location.hostname || "loadcell.local";
  ws = new WebSocket("ws://"+host+":81/");
  ws.onopen = () => { $("dot").classList.add("on"); $("conn").textContent="connected";
                      log("websocket open"); send({c:"status"}); send({c:"regs"}); };
  ws.onclose = () => { $("dot").classList.remove("on"); $("conn").textContent="offline";
                       setTimeout(connect, 1500); };
  ws.onerror = () => ws.close();
  ws.onmessage = e => {
    let m; try{ m = JSON.parse(e.data); }catch(_){ return; }
    if (m.t === "d") onData(m);
    else if (m.t === "s") onStatus(m);
    else if (m.t === "r") onRegs(m);
    else if (m.t === "log") log(m.m);
  };
}
const send = o => { if (ws && ws.readyState === 1) ws.send(JSON.stringify(o)); };

// ---------------------------------------------------------------- data
function onData(m){
  if (paused) return;
  const t0 = m.ms;
  // Wall-clock time for board-ms 0, so every sample below (however its exact
  // instant is interpolated within the batch) converts to a real timestamp.
  const wallOffset = Date.now() - t0;

  m.ch.forEach((c, ch) => {
    const st = chanState[ch];
    st.offset = c.o; st.kscale = c.k;
    // Spread this batch's samples evenly over the interval since the last
    // batch so the x-axis reflects real elapsed time, not just point index.
    const k = c.v.length;
    const dt = (st.lastMs != null && k) ? (t0 - st.lastMs) : 0;
    for (let i = 0; i < k; i++){
      const raw = c.v[i];
      const u = st.kscale ? (raw - st.offset) / st.kscale : 0;
      const sampleMs = t0 - dt * (k - 1 - i) / k;
      st.data.push(u);
      st.times.push(sampleMs);
      lastKnown[ch] = { raw, units: u };
      if (recording) {
        recRows.push([
          new Date(wallOffset + sampleMs).toISOString(),
          lastKnown[0].raw, lastKnown[0].units.toFixed(5),
          lastKnown[1].raw, lastKnown[1].units.toFixed(5),
          battV.toFixed(3), battPct
        ]);
      }
    }
    if (k) st.lastMs = t0;
    while (st.data.length > st.maxPts) { st.data.shift(); st.times.shift(); }
    if (c.v.length){
      const last = c.v[c.v.length-1];
      const lu = st.kscale ? (last - st.offset)/st.kscale : 0;
      $("val"+ch).textContent = Math.abs(lu) < 1000 ? lu.toFixed(2) : lu.toFixed(0);
      $("raw"+ch).textContent = last;
    }
    draw(ch);
  });
}

function onStatus(s){
  lastStatus = s;
  battV = s.vbat; battPct = s.pct;
  $("ip").textContent   = s.ip;
  $("rssi").textContent = s.rssi + " dBm";
  $("sps").textContent  = s.sps_act.toFixed(1) + " / " + s.sps + " SPS";
  $("up").textContent   = fmtUp(s.uptime);
  $("battxt").textContent = s.pct + "%  " + s.vbat.toFixed(2) + " V";
  const b = $("battbar");
  b.style.width = s.pct + "%";
  b.style.background = s.pct > 40 ? "var(--ok)" : (s.pct > 15 ? "var(--warn)" : "var(--bad)");
  $("gain").value   = GAINS.indexOf(s.gain);
  $("spsSel").value = Object.keys(SPSV).find(k => SPSV[k] == s.sps);
  $("ldo").value    = LDOV.indexOf(s.ldo);
  s.ch.forEach((c, ch) => {
    const st = chanState[ch];
    st.unit = c.units; $("unit"+ch).textContent = " " + st.unit;
    $("kshow"+ch).textContent = c.scale;
    $("oshow"+ch).textContent = c.offset;
    if (document.activeElement !== $("kset"+ch)) $("kset"+ch).value = c.scale;
    if (document.activeElement !== $("oset"+ch)) $("oset"+ch).value = c.offset;
    if (document.activeElement !== $("uset"+ch)) $("uset"+ch).value = c.units;
    $("avg"+ch).value = c.avg;
  });
  $("bPause") && ($("bPause").textContent = s.stream ? "Pause" : "Resume");
  paused = !s.stream;
}
const fmtUp = s => (s>=3600? Math.floor(s/3600)+"h ":"") + Math.floor(s/60)%60 + "m " + (s%60) + "s";

// ---------------------------------------------------------------- registers
function onRegs(m){
  regcache = {};
  m.v.forEach(p => regcache[p[0]] = p[1]);
  renderRegs();
}
function renderRegs(){
  const tb = document.querySelector("#regs tbody");
  tb.innerHTML = "";
  Object.keys(regcache).map(Number).sort((a,b)=>a-b).forEach(a => {
    if (keyOnly && !KEYREGS.includes(a)) return;
    const v = regcache[a];
    const tr = document.createElement("tr");
    tr.innerHTML = "<td>0x"+h2(a)+"</td><td>"+(REGNAME[a]||"-")+"</td>"+
                   "<td>0x"+h2(v)+"</td><td>"+v.toString(2).padStart(8,"0")+"</td>"+
                   "<td><input data-a='"+a+"' value='0x"+h2(v)+"'></td>";
    tb.appendChild(tr);
  });
  tb.querySelectorAll("input").forEach(inp => {
    inp.addEventListener("keydown", e => {
      if (e.key !== "Enter") return;
      const val = parseInt(inp.value.replace(/^0x/i,""), 16);
      if (isNaN(val) || val < 0 || val > 255) { log("bad byte"); return; }
      send({c:"wreg", a:+inp.dataset.a, v:val});
    });
  });
  decode();
}
const h2 = v => v.toString(16).toUpperCase().padStart(2,"0");

function decode(){
  const pu = regcache[0x00]||0, c1 = regcache[0x01]||0, c2 = regcache[0x02]||0;
  const bit = (v,b) => (v>>b)&1;
  $("decode").textContent =
   "PU_CTRL 0x"+h2(pu)+"   AVDDS="+bit(pu,7)+" OSCS="+bit(pu,6)+" CR(rdy)="+bit(pu,5)+
   " CS="+bit(pu,4)+" PUR="+bit(pu,3)+" PUA="+bit(pu,2)+" PUD="+bit(pu,1)+" RR="+bit(pu,0)+"\n"+
   "CTRL1   0x"+h2(c1)+"   CRP="+bit(c1,7)+" DRDY_SEL="+bit(c1,6)+
   " VLDO="+LDOV[(c1>>3)&7]+"V GAIN=x"+GAINS[c1&7]+"\n"+
   "CTRL2   0x"+h2(c2)+"   CHS=CH"+(bit(c2,7)?2:1)+" CRS="+(SPSV[(c2>>4)&7]||"?")+"SPS"+
   " CAL_ERR="+bit(c2,3)+" CALS="+bit(c2,2)+" CALMOD="+(c2&3);
}

// ---------------------------------------------------------------- chart (one canvas per channel)
for (let ch = 0; ch < NCHAN; ch++) {
  const cv = $("chart"+ch);
  chanState[ch].cv = cv;
  chanState[ch].cx = cv.getContext("2d");
}
function fit(){
  for (let ch = 0; ch < NCHAN; ch++) {
    const cv = chanState[ch].cv;
    cv.width = cv.clientWidth * devicePixelRatio;
    cv.height = cv.clientHeight * devicePixelRatio;
    chanState[ch].cx.setTransform(devicePixelRatio,0,0,devicePixelRatio,0,0);
    draw(ch);
  }
}
addEventListener("resize", fit);

function draw(ch){
  const st = chanState[ch], cv = st.cv, cx = st.cx, data = st.data, times = st.times;
  const W = cv.clientWidth, H = cv.clientHeight;
  const topPad = 6, botPad = 16;
  cx.clearRect(0,0,W,H);
  if (data.length < 2) return;
  let lo = Math.min(...data), hi = Math.max(...data);
  if (hi - lo < 1e-9) { hi += 1; lo -= 1; }
  const vpad = (hi - lo) * 0.12; lo -= vpad; hi += vpad;
  const y = v => H - botPad - (v - lo) / (hi - lo) * (H - topPad - botPad);

  // x-axis is real elapsed time (seconds ago), not sample index, so gaps or
  // uneven per-channel sampling show up as they actually happened.
  const tEnd = times[times.length - 1], tStart = times[0];
  const span = Math.max(tEnd - tStart, 1);   // ms, avoid div-by-zero
  const x = t => (t - tStart) / span * W;

  cx.strokeStyle = "#1d232c"; cx.lineWidth = 1;
  cx.fillStyle = "#6e7681"; cx.font = "10px ui-monospace,monospace";
  for (let i = 0; i <= 4; i++){
    const v = lo + (hi - lo) * i / 4, yy = Math.round(y(v)) + 0.5;
    cx.beginPath(); cx.moveTo(0,yy); cx.lineTo(W,yy); cx.stroke();
    cx.fillText(v.toFixed(2), 3, yy - 3);
  }
  // vertical gridlines labeled in seconds-ago
  for (let i = 0; i <= 4; i++){
    const t = tStart + span * i / 4, xx = Math.round(x(t)) + 0.5;
    const secAgo = (tEnd - t) / 1000;
    cx.beginPath(); cx.moveTo(xx, topPad); cx.lineTo(xx, H - botPad); cx.stroke();
    const label = (secAgo < 0.05 ? "0" : "-" + secAgo.toFixed(1)) + "s";
    const tw = cx.measureText(label).width;
    cx.fillText(label, Math.min(Math.max(xx - tw/2, 0), W - tw), H - 4);
  }

  cx.strokeStyle = "#2f81f7"; cx.lineWidth = 1.6; cx.beginPath();
  data.forEach((v,i) => { const xx = x(times[i]);
                          i ? cx.lineTo(xx, y(v)) : cx.moveTo(xx, y(v)); });
  cx.stroke();
  cx.fillStyle = "#6e7681";
  cx.fillText(data.length + " pts / " + (span/1000).toFixed(1) + "s   " + st.unit, W - 150, 12);
}

// ---------------------------------------------------------------- controls
for (let ch = 0; ch < NCHAN; ch++) {
  const st = chanState[ch];
  $("bTare"+ch).onclick = () => send({c:"tare", ch});
  $("bCal"+ch).onclick  = () => send({c:"calw", ch, v:parseFloat($("calw"+ch).value)});
  $("bK"+ch).onclick    = () => send({c:"scale", ch, v:parseFloat($("kset"+ch).value)});
  $("bO"+ch).onclick    = () => send({c:"offset", ch, v:parseInt($("oset"+ch).value)});
  $("bU"+ch).onclick    = () => send({c:"units", ch, v:$("uset"+ch).value.slice(0,7)});
  $("avg"+ch).onchange  = e => send({c:"avg", ch, v:+e.target.value});
  $("bClear"+ch).onclick = () => { st.data = []; st.times = []; st.lastMs = null; draw(ch); };
}

$("bRec").onclick = () => {
  recording = !recording;
  if (recording) recRows = [];
  $("bRec").textContent = recording ? "Stop recording" : "Record";
  $("bRec").classList.toggle("pri", recording);
};
$("bSave").onclick = () => {
  if (!recRows.length) { log("nothing recorded"); return; }
  const lines = [];
  lines.push("# Wireless Strain Gauge recording");
  lines.push("# exported " + new Date().toISOString());
  if (lastStatus) {
    lines.push(`# gain x${lastStatus.gain}  sps ${lastStatus.sps} (actual ${lastStatus.sps_act.toFixed(1)})  ldo ${lastStatus.ldo}V`);
    lastStatus.ch.forEach((c, ch) => {
      lines.push(`# ch${ch+1}: offset=${c.offset} scale=${c.scale} units=${c.units} avg=${c.avg}`);
    });
  }
  lines.push("#");
  lines.push("timestamp,ch1_raw,ch1_units,ch2_raw,ch2_units,battery_v,battery_pct");
  recRows.forEach(r => lines.push(r.join(",")));
  const csv = lines.join("\n");
  const a = document.createElement("a");
  a.href = URL.createObjectURL(new Blob([csv], {type:"text/csv"}));
  a.download = "loadcell_" + Date.now() + ".csv";
  a.click();
};
setInterval(() => { $("recinfo").textContent = recording ? recRows.length+" rows" : ""; }, 500);

$("bAfe").onclick   = () => send({c:"afecal"});
$("bRst").onclick   = () => send({c:"reset"});
$("gain").onchange  = e => send({c:"gain", v:+e.target.value});
$("spsSel").onchange= e => send({c:"sps",  v:+e.target.value});
$("ldo").onchange   = e => send({c:"ldo",  v:+e.target.value});
$("bRefresh").onclick = () => send({c:"regs"});
$("span").onchange  = e => { const n = +e.target.value; chanState.forEach(st => st.maxPts = n); };
$("bPause").onclick = () => send({c:"stream", v:paused});
$("tAll").onclick   = () => { keyOnly=false; $("tAll").classList.add("act");
                              $("tKey").classList.remove("act"); renderRegs(); };
$("tKey").onclick   = () => { keyOnly=true;  $("tKey").classList.add("act");
                              $("tAll").classList.remove("act"); renderRegs(); };

fit();
connect();
</script>
</body>
</html>
)PAGE";
