// RTK1010 Router — Preact + HTM single-page app (theme from esp32-web-interface).
const { h, render } = preact;
const { useState, useEffect, useRef } = preactHooks;
const html = htm.bind(h);

const C = { acc:'#4cc9f0', grn:'#54e6a4', amb:'#ffb454', red:'#ff6b6b', pur:'#b78cff', blu:'#5b9dff' };
const RNAMES = ['GPS','GLO','GAL','SBAS','QZSS','BDS','NavIC'];
const GNAMES = ['GPS','GLO','GAL','BDS','QZSS','NavIC'];
const FIX = ['No fix','GPS','DGPS','PPS','RTK Fixed','RTK Float','Dead reckoning','Manual','Simulation'];
const FIXMODE = { 2:'2D', 3:'3D' };
const AIR = ['2.4k','2.4k','2.4k','4.8k','9.6k','19.2k','38.4k','62.5k'];
const UBP = ['1200','2400','4800','9600','19200','38400','57600','115200'];
const PWR = ['22 dBm','17 dBm','13 dBm','10 dBm'];
const SUBP = ['200 B','128 B','64 B','32 B'];
const GBAUDS = [9600,19200,38400,57600,115200,230400,460800,921600,1000000,2000000];
const LBAUDS = [9600,19200,38400,57600,115200];
const GPSMODBAUDS = [115200,230400,460800,921600,3000000]; // RTK-1010 PAIR864-supported port bauds
const DGPS = ['Off','RTCM','SBAS','SLAS'];
const FIXINT = [{ms:1000,lbl:'1000 ms (1 Hz)'},{ms:500,lbl:'500 ms (2 Hz)'},{ms:200,lbl:'200 ms (5 Hz)'},{ms:100,lbl:'100 ms (10 Hz)'}];
const PAR = ['8N1','8O1','8E1','8N1'];          // E220 parity (SPED bits 3-4)
const TRANM = ['Normal','Fixed'];               // E220 transmission mode (fixed-point addressing)
const WOR = ['500ms','1000ms','1500ms','2000ms','2500ms','3000ms','3500ms','4000ms'];
const SATSYS = ['GPS','GLO','GAL','BDS','QZSS','NavIC'];

const gj = (u) => fetch(u).then(r => r.json());
const gt = (u) => fetch(u).then(r => r.text());
const brk = (a, nm) => !a ? '' : a.map((v,i)=>v>0?nm[i]+' '+v:null).filter(Boolean).join(', ');
const fixTxt = (f) => FIX[f] !== undefined ? FIX[f] : f;
const fixLabel = (st) => { const q = fixTxt(st.fix); const d = FIXMODE[st.fixmode]; return d ? q + ' · ' + d : q; };
const opt = (a) => a.map((v,i) => html`<option value=${i}>${v}</option>`);

if (typeof Chart !== 'undefined') {
  Chart.defaults.font.family = 'system-ui, sans-serif';
  Chart.defaults.font.size = 10;
  Chart.defaults.color = '#9aa3b2';
  Chart.defaults.borderColor = 'rgba(127,146,167,.14)';
}

// ---- streaming line chart ----
function Sig({ label, unit, series, hist, zero = true }) {
  const cv = useRef(), ch = useRef();
  useEffect(() => {
    ch.current = new Chart(cv.current, {
      type: 'line',
      // zero=false (e.g. RSSI, all-negative): fill to the bottom of the scale, not to y=0
      // (filling to 0 made the area render upside-down), and let the axis auto-range.
      data: { labels: [], datasets: series.map(s => ({ label: s.label, data: [], borderColor: s.c, backgroundColor: s.c+'22', fill: zero ? 'origin' : 'start', pointRadius: 0, tension: .25, borderWidth: 2 })) },
      options: { animation: false, responsive: true, maintainAspectRatio: false,
        plugins: { legend: { display: series.length > 1, labels: { boxWidth: 10, usePointStyle: true } } },
        scales: { x: { display: false }, y: { beginAtZero: zero, ticks: { maxTicksLimit: 4 } } } }
    });
    return () => ch.current && ch.current.destroy();
  }, []);
  useEffect(() => {
    const c = ch.current; if (!c) return;
    c.data.labels = hist.map((_, i) => i);
    series.forEach((s, i) => { c.data.datasets[i].data = hist.map(x => x[s.k]); });
    c.update('none');
  }, [hist]);
  return html`<div class="dash-box compact"><h3>${label}${unit ? html` <span style="color:var(--text3);font-weight:400">${unit}</span>` : ''}</h3>
    <div style="height:130px"><canvas ref=${cv}></canvas></div></div>`;
}

// ---- per-constellation bar chart ----
function Bar({ label, labels, data, color }) {
  const cv = useRef(), ch = useRef();
  useEffect(() => {
    ch.current = new Chart(cv.current, {
      type: 'bar',
      data: { labels, datasets: [{ data: data || [], backgroundColor: color, borderRadius: 4, maxBarThickness: 36 }] },
      options: { animation: false, responsive: true, maintainAspectRatio: false,
        plugins: { legend: { display: false }, tooltip: { displayColors: false } },
        scales: { x: { grid: { display: false } }, y: { beginAtZero: true, ticks: { maxTicksLimit: 5, precision: 0 } } } }
    });
    return () => ch.current && ch.current.destroy();
  }, []);
  useEffect(() => { const c = ch.current; if (!c) return; c.data.labels = labels; c.data.datasets[0].data = data || []; c.update('none'); });
  const tot = (data || []).reduce((a, b) => a + (b || 0), 0);
  return html`<div class="dash-box compact"><h3>${label} <span style="float:right;font-weight:400;color:var(--text3)">${tot} total</span></h3>
    <div style="height:150px"><canvas ref=${cv}></canvas></div></div>`;
}

// ---- per-satellite C/N0 signal bars (filled = used in fix, outline = tracked only) ----
function SatBars({ sats }) {
  const cv = useRef(), ch = useRef();
  const SCOL = [C.acc, C.grn, C.amb, C.red, C.pur, C.blu];
  useEffect(() => {
    ch.current = new Chart(cv.current, {
      type: 'bar',
      data: { labels: [], datasets: [{ data: [], backgroundColor: [], borderColor: [], borderWidth: 1.5, borderRadius: 3 }] },
      options: { animation: false, responsive: true, maintainAspectRatio: false,
        plugins: { legend: { display: false }, tooltip: { displayColors: false, callbacks: {
          label: (c) => c.raw + ' dB-Hz' + (c.dataset._u && c.dataset._u[c.dataIndex] ? ' · used' : ' · tracked') } } },
        scales: { x: { grid: { display: false }, ticks: { font: { size: 8 }, autoSkip: false, maxRotation: 90, minRotation: 90 } },
                  y: { beginAtZero: true, suggestedMax: 55, ticks: { maxTicksLimit: 6 } } } }
    });
    return () => ch.current && ch.current.destroy();
  }, []);
  useEffect(() => {
    const c = ch.current; if (!c) return;
    const ss = (sats || []).slice().sort((a, b) => (a.s - b.s) || (a.v - b.v));
    c.data.labels = ss.map(x => x.v);
    c.data.datasets[0].data = ss.map(x => x.c);
    c.data.datasets[0].backgroundColor = ss.map(x => x.u ? (SCOL[x.s] || C.acc) : 'transparent');
    c.data.datasets[0].borderColor = ss.map(x => SCOL[x.s] || C.acc);
    c.data.datasets[0]._u = ss.map(x => !!x.u);
    c.update('none');
  });
  const used = (sats || []).filter(x => x.u).length;
  return html`<div class="dash-box compact">
    <h3>Satellite signal (C/N0) <span style="float:right;font-weight:400;color:var(--text3)">${used} used / ${(sats||[]).length} seen</span></h3>
    <div style="display:flex;gap:12px;flex-wrap:wrap;margin-bottom:6px">${SATSYS.map((n,i) => html`<span style="font-size:.68rem;color:var(--text3)"><span style=${'display:inline-block;width:9px;height:9px;border-radius:2px;vertical-align:middle;margin-right:4px;background:'+SCOL[i]}></span>${n}</span>`)}</div>
    <div style="height:180px"><canvas ref=${cv}></canvas></div>
    <p style="color:var(--text3);font-size:.68rem;margin:.3rem 0 0">Filled bar = used in fix · outline = tracked but not used. Per-satellite signal comes from the GPS GSV/GSA sentences (RTCM corrections don't carry per-sat C/N0).</p></div>`;
}

// ---- autoscaling position scatter (offsets from running average, last N fixes) ----
function PosScatter({ pts, onReset }) {
  const cv = useRef(), ch = useRef();
  useEffect(() => {
    ch.current = new Chart(cv.current, {
      type: 'scatter',
      data: { datasets: [
        { data: [], pointRadius: 2, pointBackgroundColor: C.acc + 'aa', showLine: false },
        { data: [{x:0,y:0}], pointRadius: 6, pointStyle: 'crossRot', borderColor: C.amb, borderWidth: 2 }
      ]},
      options: { animation: false, responsive: true, maintainAspectRatio: true, aspectRatio: 1,
        plugins: { legend: { display: false }, tooltip: { enabled: false } },
        scales: { x: { title: { display: true, text: 'East (m)' }, ticks: { maxTicksLimit: 6 }, grid: { color: 'rgba(127,146,167,.12)' } },
                  y: { title: { display: true, text: 'North (m)' }, ticks: { maxTicksLimit: 6 }, grid: { color: 'rgba(127,146,167,.12)' } } } }
    });
    return () => ch.current && ch.current.destroy();
  }, []);
  useEffect(() => {
    const c = ch.current; if (!c) return;
    const n = pts.length;
    if (!n) { c.data.datasets[0].data = []; c.update('none'); return; }
    let mlat = 0, mlon = 0; for (const p of pts) { mlat += p.lat; mlon += p.lon; } mlat /= n; mlon /= n;
    const mLat = 111320, mLon = 111320 * Math.cos(mlat * Math.PI / 180);
    const d = pts.map(p => ({ x: (p.lon - mlon) * mLon, y: (p.lat - mlat) * mLat }));
    let max = 0.05; for (const q of d) max = Math.max(max, Math.abs(q.x), Math.abs(q.y)); max *= 1.15;
    c.options.scales.x.min = -max; c.options.scales.x.max = max;
    c.options.scales.y.min = -max; c.options.scales.y.max = max;
    c.data.datasets[0].data = d;
    c.update('none');
  });
  let spread = null;
  if (pts.length > 1) {
    let mlat = 0, mlon = 0; for (const p of pts) { mlat += p.lat; mlon += p.lon; } mlat /= pts.length; mlon /= pts.length;
    const mLat = 111320, mLon = 111320 * Math.cos(mlat * Math.PI / 180);
    let mx = 0; for (const p of pts) mx = Math.max(mx, Math.hypot((p.lon - mlon) * mLon, (p.lat - mlat) * mLat));
    spread = mx;
  }
  return html`<div class="dash-box compact">
    <h3>Position scatter <span style="float:right;font-weight:400;color:var(--text3)">${pts.length} pts${spread != null ? ' · max ±' + spread.toFixed(2) + ' m' : ''} <button onClick=${onReset} style="margin-left:8px">Reset</button></span></h3>
    <div style="max-width:360px;margin:0 auto"><canvas ref=${cv}></canvas></div>
    <p style="color:var(--text3);font-size:.68rem;margin:.3rem 0 0">Each dot is a fix offset from the running average (✕ at centre). Auto-scales; keeps the last 500 fixes.</p></div>`;
}

// ---- live raw stream console ----
function Console({ src, label, defaultHex }) {
  const [txt, setTxt] = useState('');
  const [hex, setHex] = useState(!!defaultHex);
  const [paused, setPaused] = useState(false);
  const cur = useRef(0), box = useRef();
  useEffect(() => {
    let stop = false;
    const tick = async () => {
      if (!paused) try {
        const r = await fetch('/console?src=' + src + '&since=' + cur.current);
        const nx = r.headers.get('X-Next'); const b = new Uint8Array(await r.arrayBuffer());
        if (nx !== null) cur.current = +nx;
        if (b.length) {
          let s = '';
          if (hex) for (const x of b) s += x.toString(16).padStart(2,'0') + ' ';
          else for (const x of b) s += x===10?'\n':(x>=32&&x<127)?String.fromCharCode(x):'.';
          setTxt(t => (t + s).slice(-8000));
        }
      } catch (e) {}
      if (!stop) setTimeout(tick, 500);
    };
    tick();
    return () => { stop = true; };
  }, [src, hex, paused]);
  useEffect(() => { if (box.current && !paused) box.current.scrollTop = box.current.scrollHeight; });
  return html`<div class="dash-box compact"><h3>${label}
    <span style="float:right;font-weight:400">
      <button onClick=${() => setHex(v=>!v)}>${hex?'ASCII':'Hex'}</button>
      <button onClick=${() => setPaused(v=>!v)}>${paused?'Resume':'Pause'}</button>
      <button onClick=${() => { setTxt(''); }}>Clear</button></span></h3>
    <pre ref=${box} style="height:200px;overflow:auto;background:var(--bg);padding:8px;border-radius:8px;font-family:var(--mono);font-size:12px;white-space:pre-wrap;word-break:break-all;margin:0">${txt}</pre></div>`;
}

// ---- Dashboard ----
function Dashboard({ st, hist, sats, posHist, onResetPos }) {
  if (!st) return html`<div class="tabdiv" style="display:block">Loading…</div>`;
  const MODES = { gps:'USB ⇄ GPS', lora:'USB ⇄ LoRa', rover:'RTK Rover', hybrid:'Hybrid' };
  const last = hist.length ? hist[hist.length - 1] : {};
  const gB = last.gpsBps || 0, lB = last.loraBps || 0;
  const mb = (id, lbl) => html`<button onClick=${() => fetch('/mode?set='+id)}
    style=${st.mode===id ? 'background:var(--accent);border-color:var(--accent);color:#04222e;font-weight:700' : ''}>${lbl}</button>`;
  return html`<div class="tabdiv" style="display:block">
    <div class="dash-box hero-card">
      <div class="hero-top">
        <span class="metric-label">Active mode</span>
        <div class="hero-state">${MODES[st.mode] || st.mode}</div>
        <span class=${'pill ' + (st.fix>=4?'active':st.fix>0?'info':'danger')}><span class=dot></span>${fixLabel(st)} · ${st.sats} sats · HDOP ${st.hdop}</span>
      </div>
      <div style="display:flex;gap:6px;flex-wrap:wrap;margin-top:.2rem">${mb('gps','USB ⇄ GPS')} ${mb('lora','USB ⇄ LoRa')} ${mb('rover','RTK Rover')} ${mb('hybrid','Hybrid')}</div>
      <p class="hero-sub" style="margin:.2rem 0 0">Both GPS & LoRa are decoded in every mode — the buttons only change what reaches USB/TCP.</p>
      <div style="display:flex;gap:8px;flex-wrap:wrap;margin-top:.7rem">
        <span class=${'pill '+(gB>0?'active':'muted')}><span class=dot></span>GPS ${gB>0?'▸ '+gB+' B/s':'no data'}</span>
        <span class=${'pill '+(lB>0?'active':'muted')}><span class=dot></span>LoRa ${lB>0?'▸ '+lB+' B/s':'no data'}</span>
        <span class=${'pill '+(st.cdcconn?'active':'muted')}><span class=dot></span>USB-CDC ${st.cdcconn?'DTR':'no DTR'}</span>
        <span class=${'pill '+(st.tcp?'active':'muted')}><span class=dot></span>TCP ${st.tcp?'client':'idle'}</span>
      </div>
      <div class="hero-metrics" style="margin-top:.7rem">
        <div class="metric"><span class="metric-label">Fix</span><span class="metric-value" style="font-size:1.05rem">${fixLabel(st)}</span></div>
        <div class="metric"><span class="metric-label">HDOP</span><span class="metric-value">${st.hdop}</span></div>
        <div class="metric"><span class="metric-label">Output rate</span><span class="metric-value">${st.gpshz}<span class="metric-unit">Hz</span></span></div>
        <div class="metric"><span class="metric-label">GPS sats</span><span class="metric-value">${st.sats}</span></div>
        <div class="metric"><span class="metric-label">RTCM sats</span><span class="metric-value">${st.rtcmsats}</span></div>
        <div class="metric"><span class="metric-label">WiFi RSSI</span><span class="metric-value">${st.rssi}<span class="metric-unit">dBm</span></span></div>
      </div>
    </div>

    <div style="display:grid;grid-template-columns:repeat(auto-fit,minmax(260px,1fr));gap:1rem;margin-top:1rem">
      <div class="dash-box compact"><h3>Network</h3><table>
        <tr><td>STA</td><td>${st.sta} @ ${st.staip}</td></tr>
        <tr><td>AP</td><td>${st.ap} · ${st.apclients} client(s)</td></tr>
        <tr><td>TCP NMEA</td><td>rtk1010.local:10110</td></tr></table></div>
      <div class="dash-box compact"><h3>GPS (used)</h3><table>
        <tr><td>Fix</td><td>${fixLabel(st)} · ${st.sats} sats</td></tr>
        <tr><td>HDOP</td><td>${st.hdop}</td></tr>
        <tr><td>By const</td><td>${brk(st.gpsbyc, GNAMES) || '—'}</td></tr>
        <tr><td>Baud / pins</td><td>${st.gpsbaud} · GPIO${st.gtx}/${st.grx}</td></tr></table></div>
      <div class="dash-box compact"><h3>RTCM (LoRa)</h3><table>
        <tr><td>Frames</td><td>${st.rtcmcount} · last type ${st.rtcmtype||'—'}</td></tr>
        <tr><td>Sats</td><td>${st.rtcmsats} total / ${st.rtcmcons} cons</td></tr>
        <tr><td>By const</td><td>${brk(st.rtcmbyc, RNAMES) || '—'}</td></tr></table></div>
    </div>

    <div style="margin-top:1rem"><${PosScatter} pts=${posHist} onReset=${onResetPos} /></div>

    <div style="margin-top:1rem"><${SatBars} sats=${sats} /></div>

    <div style="margin-top:1rem"><${Bar} label="RTCM satellites by constellation" labels=${RNAMES} data=${st.rtcmbyc} color=${C.grn + 'cc'} /></div>

    <div style="display:grid;grid-template-columns:repeat(auto-fit,minmax(280px,1fr));gap:1rem;margin-top:1rem">
      <${Sig} label="Fix quality" unit="0-5" hist=${hist} series=${[{k:'fix',label:'fix',c:C.grn}]} />
      <${Sig} label="HDOP (precision, lower better)" hist=${hist} series=${[{k:'hdop',label:'HDOP',c:C.amb}]} />
      <${Sig} label="Satellites" hist=${hist} series=${[{k:'gpsSats',label:'GPS used',c:C.acc},{k:'rtcmSats',label:'RTCM',c:C.grn}]} />
      <${Sig} label="Throughput" unit="B/s" hist=${hist} series=${[{k:'gpsBps',label:'GPS',c:C.acc},{k:'loraBps',label:'LoRa',c:C.amb},{k:'usbBps',label:'USB/TCP',c:C.pur}]} />
      <${Sig} label="WiFi RSSI" unit="dBm" hist=${hist} zero=${false} series=${[{k:'rssi',label:'RSSI',c:C.blu}]} />
    </div></div>`;
}

// ---- RTK-1010 config ----
function GpsTab({ st }) {
  const [resp, setResp] = useState('');
  const [cmd, setCmd] = useState('');
  const cur = useRef(0);
  const pull = async () => { try { const r = await fetch('/console?src=resp&since='+cur.current); const nx=r.headers.get('X-Next'); const b=new Uint8Array(await r.arrayBuffer()); if(nx!==null)cur.current=+nx; if(b.length){let s='';for(const x of b)s+=x===10?'\n':(x>=32&&x<127)?String.fromCharCode(x):'.';setResp(t=>(t+s).slice(-4000));} } catch(e){} };
  useEffect(() => { const id = setInterval(pull, 700); return () => clearInterval(id); }, []);
  const send = (c) => { if(c) fetch('/gpscmd?cmd=' + encodeURIComponent(c)); };
  const [bmsg, setBmsg] = useState('');
  const [smsg, setSmsg] = useState('');
  const setBaud = async (b) => { setBmsg('changing to ' + b + ' & rebooting GPS…'); try { const r = await fetch('/gpsbaud?baud=' + b); setBmsg(r.ok ? ('now ' + b + ' baud') : 'unsupported baud'); } catch (e) { setBmsg('failed'); } };
  const save = async () => { setSmsg('saving — GPS stops briefly…'); try { const r = await fetch('/gpssave'); setSmsg(r.ok ? 'saved to flash' : 'save failed'); } catch (e) { setSmsg('failed'); } };
  const reset = async () => { if (!confirm('Restore the RTK-1010 to FACTORY DEFAULTS?\n\nThis clears all GPS configuration (fix rate, RTK/DGPS, etc.) and may reset the module baud rate. GPS output stops for a few seconds while the link re-syncs.')) return; setSmsg('restoring defaults — please wait…'); try { const r = await fetch('/gpsreset'); const t = await r.text(); setSmsg(r.ok ? ('defaults restored (' + t + ')') : 'reset failed'); } catch (e) { setSmsg('failed'); } };
  // "Read all" issues the queryable settings; replies stream into the log below and parse here.
  const readAll = () => { setResp(''); send('PLSC,VER'); setTimeout(() => send('PAIR051'), 200); setTimeout(() => send('PAIR401'), 400); };
  // parsed replies: $PLSR,VER,RTK35X,…   $PLSR,FIXRATE,5   $PAIR051,200 (fix interval ms)   $PAIR401,1
  const verm = (resp.match(/\$PLSR,VER,([^*\r\n]+)/g) || []).pop();
  const ver = verm ? verm.replace('$PLSR,VER,', '').split(',').slice(0, 2).join(' · ') : null;
  const frm = (resp.match(/\$PLSR,FIXRATE,(\d+)/g) || []).pop();
  const frHz = frm ? frm.split(',')[2] : null;
  const fim = (resp.match(/\$PAIR05[01],(\d+)/g) || []).pop();
  const fiMs = fim ? +fim.split(',')[1] : null;
  const dgm = (resp.match(/\$PAIR401,(\d+)/g) || []).pop();
  const dg = dgm ? DGPS[+dgm.split(',')[1]] : null;
  return html`<div class="tabdiv" style="display:block">
    <div class="dash-box compact">
      <h3>Current settings <span style="float:right;font-weight:400"><button onClick=${readAll}>Read all</button></span></h3>
      <table>
        <tr><td style="width:150px">Firmware</td><td>${ver || '— press Read all'}</td></tr>
        <tr><td>Fix interval</td><td>${fiMs ? fiMs + ' ms (' + (1000/fiMs).toFixed(fiMs<1000?1:0) + ' Hz)' : '— press Read all'}</td></tr>
        <tr><td>Output rate</td><td>${st ? st.gpshz + ' Hz (measured live)' : '—'}${frHz ? ' · set ' + frHz + ' Hz' : ''}</td></tr>
        <tr><td>DGPS source</td><td>${dg || '— press Read all'}</td></tr>
        <tr><td>ESP GPS baud</td><td>${st ? st.gpsbaud : ''}</td></tr></table></div>

    <div style="display:grid;grid-template-columns:repeat(auto-fit,minmax(230px,1fr));gap:1rem;margin-top:1rem">
      <div class="dash-box compact"><h3>Fix interval (PAIR050)</h3>
        <p style="color:var(--text2);font-size:.78rem;margin:.2rem 0 .4rem">Internal position-fix interval (how often a fix is computed), 100–1000 ms.</p>
        <div style="display:flex;gap:6px"><select id="fixint">${FIXINT.map(o => html`<option value=${o.ms}>${o.lbl}</option>`)}</select>
        <button onClick=${() => send('PAIR050,' + document.getElementById('fixint').value)}>Set</button></div></div>

      <div class="dash-box compact"><h3>Output rate (FIXRATE)</h3>
        <p style="color:var(--text2);font-size:.78rem;margin:.2rem 0 .4rem">NMEA output update rate — separate from the fix interval above.</p>
        <div style="display:flex;gap:6px;flex-wrap:wrap">
          <button onClick=${() => send('PLSC,FIXRATE,1')}>1 Hz</button>
          <button onClick=${() => send('PLSC,FIXRATE,5')}>5 Hz</button>
          <button onClick=${() => send('PLSC,FIXRATE,10')}>10 Hz</button></div></div>

      <div class="dash-box compact"><h3>RTK mode</h3>
        <div style="display:flex;gap:6px;flex-wrap:wrap">
          <button onClick=${() => send('PLSC,MCBASE,0')}>Rover</button>
          <button onClick=${() => send('PLSC,MCBASE,1')}>Base</button></div></div>

      <div class="dash-box compact"><h3>DGPS / RTCM source</h3>
        <div style="display:flex;gap:6px;flex-wrap:wrap">
          <button onClick=${() => send('PAIR400,1')}>RTCM</button>
          <button onClick=${() => send('PAIR400,2')}>SBAS</button>
          <button onClick=${() => send('PAIR400,0')}>Off</button></div></div>

      <div class="dash-box compact"><h3>Baud rate${bmsg?html` <span style="float:right;font-weight:400;color:var(--green)">${bmsg}</span>`:''}</h3>
        <p style="color:var(--text2);font-size:.78rem;margin:.2rem 0 .4rem">Saves to the module's flash, reboots it, then matches the ESP UART. Only these rates are supported.</p>
        <div style="display:flex;gap:6px"><select id="grb">${GPSMODBAUDS.map(b => html`<option>${b}</option>`)}</select>
        <button onClick=${() => setBaud(document.getElementById('grb').value)}>Set & reboot</button></div></div>
    </div>

    <div class="dash-box compact" style="margin-top:1rem"><h3>Save ${smsg?html`<span style="float:right;font-weight:400;color:var(--green)">${smsg}</span>`:''}</h3>
      <p style="color:var(--text2);font-size:.78rem;margin:.2rem 0 .4rem">Fix interval / output rate / RTK mode / DGPS live in RAM until saved. At multi-Hz the module can only save with the GNSS powered off, so this powers it off, saves, and powers it back on — GPS output stops for ~1 s. (Baud is saved automatically by its own button.)</p>
      <button onClick=${save}>Save all settings to flash</button>
      <button onClick=${reset} style="margin-left:8px;border-color:var(--red);color:var(--red)">Restore factory defaults…</button></div>

    <div class="dash-box compact" style="margin-top:1rem"><h3>Send command</h3>
      <input value=${cmd} onInput=${e => setCmd(e.target.value)} placeholder="LOCOSYS cmd, e.g. PAIR062,0,1 (no \$ / checksum)" />
      <button onClick=${() => send(cmd)}>Send</button></div>

    <div class="dash-box compact" style="margin-top:1rem"><h3>Replies <span style="float:right;font-weight:400"><button onClick=${() => setResp('')}>Clear</button></span></h3>
      <pre style="height:180px;overflow:auto;background:var(--bg);padding:8px;border-radius:8px;font-family:var(--mono);font-size:12px;white-space:pre-wrap;margin:0">${resp || '(send a query — $P… replies appear here)'}</pre></div></div>`;
}

// ---- LoRa E220 config ----
function LoraTab({ st }) {
  // Defaults so the form always renders (no auto-read on mount → never gets stuck and
  // doesn't interrupt the LoRa link on every visit). Mode pins prefill from /status.
  const DEF = { ok:false, addr:0, ch:18, ubaud:7, par:0, air:7, sub:0, pwr:2, rn:0, lbt:0, rb:0, fx:0, wor:3, key:0,
                m0: st ? st.em0 : 34, m1: st ? st.em1 : 36, aux: st ? st.eaux : 37 };
  const [cfg, setCfg] = useState(false);     // config-mode switch (gate before reading/editing)
  const [d, setD] = useState(DEF);
  const [msg, setMsg] = useState('');
  const f = useRef(DEF);
  const apply = (o) => { const m = { ...f.current, ...o }; f.current = m; setD(m); };
  const set = (k, v) => { f.current = { ...f.current, [k]: v }; setD({ ...f.current }); };
  const read = async () => { setMsg('reading…'); try { const r = await gj('/loracfg'); apply(r); setMsg(r.ok ? 'read OK' : 'no response — check wiring & M0/M1/AUX pins'); } catch (e) { setMsg('request failed: ' + e.message); } };
  const write = async () => { setMsg('writing…'); const o = f.current; const p = new URLSearchParams({ write:1,
    addr:o.addr||0, ch:o.ch||0, air:o.air||0, ubaud:o.ubaud||0, par:o.par||0, pwr:o.pwr||0, sub:o.sub||0,
    rn:o.rn?1:0, lbt:o.lbt?1:0, rb:o.rb?1:0, fx:o.fx?1:0, wor:o.wor||0, key:o.key||0, m0:o.m0, m1:o.m1, aux:o.aux });
    try { const r = await gj('/loracfg?' + p); apply(r); setMsg(r.ok ? 'write OK' : 'no response — check wiring'); } catch (e) { setMsg('request failed: ' + e.message); } };
  const toggleCfg = (on) => { setCfg(on); if (on) read(); else setMsg('transparent mode'); };
  const dis = !cfg;
  const freq = (850.125 + (+d.ch || 0)).toFixed(3);
  const lbl = (t) => html`<label style="display:block;font-size:.74rem;color:var(--text2);margin:.5rem 0 .15rem">${t}</label>`;
  const sel = (k, arr) => html`<select disabled=${dis} value=${d[k]} onChange=${e => set(k, +e.target.value)} style="width:100%">${opt(arr)}</select>`;
  const enDis = (k) => html`<select disabled=${dis} value=${d[k]?1:0} onChange=${e => set(k, +e.target.value)} style="width:100%"><option value=0>Disable</option><option value=1>Enable</option></select>`;
  const num = (k) => html`<input type=number disabled=${dis} value=${d[k]} onInput=${e => set(k, +e.target.value)} style="width:100%" />`;
  return html`<div class="tabdiv" style="display:block"><div class="dash-box compact">
    <h3>LoRa radio (E220-900T22D)
      <span class=${'pill ' + (d.ok ? 'active' : 'muted')} style="float:right"><span class=dot></span>${msg || (cfg ? 'config mode' : 'transparent')}</span></h3>
    <label class="toggle-row" style="max-width:360px;margin:.3rem 0 1rem">
      <span class="toggle-label">Config mode — briefly drops the LoRa link to talk to the radio</span>
      <span class="switch"><input type=checkbox checked=${cfg} onChange=${e => toggleCfg(e.target.checked)} /><span class="slider"></span></span>
    </label>
    <div style="display:grid;grid-template-columns:repeat(auto-fit,minmax(150px,1fr));gap:0 18px;opacity:${dis?.55:1}">
      <div>
        ${lbl('Baud Rate')}${sel('ubaud', UBP)}
        ${lbl('Parity')}${sel('par', PAR)}
        ${lbl('Air Rate')}${sel('air', AIR)}
        ${lbl('Packet Size')}${sel('sub', SUBP)}
      </div>
      <div>
        ${lbl('Tran Mode')}${sel('fx', TRANM)}
        ${lbl('Wor Cycle')}${sel('wor', WOR)}
        ${lbl('Power')}${sel('pwr', PWR)}
      </div>
      <div>
        ${lbl('Channel RSSI')}${enDis('rn')}
        ${lbl('LBT')}${enDis('lbt')}
        ${lbl('Packet RSSI')}${enDis('rb')}
      </div>
      <div>
        ${lbl('Address (0-65535)')}${num('addr')}
        ${lbl(`Channel 0-80 → ${freq} MHz`)}${num('ch')}
        ${lbl('Key (0-65535, write-only)')}${num('key')}
      </div>
    </div>
    <div style="margin-top:1rem;display:flex;gap:8px;align-items:center;flex-wrap:wrap">
      <button disabled=${dis} onClick=${read}>Read</button>
      <button disabled=${dis} onClick=${write}>Write</button>
      <span style="color:var(--text2);font-size:.78rem">${dis ? 'Turn on Config mode to read/edit the radio.' : 'Read, edit, then Write. Each access briefly switches the radio to config mode.'}</span>
    </div>
    ${lbl('Mode pins M0 / M1 / AUX (GPIO; AUX=-1 disables — also on the System tab)')}
    <div style="display:flex;gap:8px;max-width:280px">${num('m0')}${num('m1')}${num('aux')}</div>
  </div></div>`;
}

// ---- Consoles ----
// LoRa carries binary RTCM3 and host-in is often binary too, so those default
// to hex (ASCII rendering of binary looks like garbage). GPS is NMEA → ASCII.
function Consoles() {
  return html`<div class="tabdiv" style="display:block">
    <${Console} src="gps" label="GPS (RTK-1010 RX, NMEA)" />
    <div style="margin-top:1rem"><${Console} src="lora" label="LoRa RX (RTCM, binary)" defaultHex=${true} /></div>
    <div style="margin-top:1rem"><${Console} src="host" label="Host → device (USB/TCP)" defaultHex=${true} /></div></div>`;
}

// ---- System ----
function SystemTab({ st }) {
  const [w, setW] = useState({});
  const [msg, setMsg] = useState('');
  const sv = (k, v) => setW({ ...w, [k]: v });
  const ports = () => {
    const g = (id) => document.getElementById(id).value;
    const p = new URLSearchParams({ gpsbaud:g('sgb'), lorabaud:g('slb'), gtx:g('sgtx'), grx:g('sgrx'), ltx:g('sltx'), lrx:g('slrx') });
    const lp = new URLSearchParams({ m0:g('sm0'), m1:g('sm1'), aux:g('saux') }); // E220 mode pins (persisted by /loracfg)
    Promise.all([fetch('/ports?'+p), fetch('/loracfg?'+lp)]).then(()=>setMsg('ports + LoRa pins applied'));
  };
  const wifi = () => { const p = new URLSearchParams({ stassid:w.ss||'', stapass:w.sp||'', apssid:w.as||'', appass:w.ap||'' }); fetch('/wifi',{method:'POST',body:p}); setMsg('saved, rebooting…'); };
  if (!st) return html`<div class="tabdiv" style="display:block">Loading…</div>`;
  const upFmt = (s) => { s = +s || 0; const h = Math.floor(s/3600), m = Math.floor((s%3600)/60), x = s%60; return (h?h+'h ':'') + ((h||m)?m+'m ':'') + x + 's'; };
  return html`<div class="tabdiv" style="display:block">
    <div style="display:grid;grid-template-columns:repeat(auto-fit,minmax(300px,1fr));gap:1rem">
      <div class="dash-box compact"><h3>Device</h3><table>
        <tr><td style="width:120px">Uptime</td><td>${upFmt(st.up)}</td></tr>
        <tr><td>Free heap</td><td>${st.heap!=null?(st.heap/1024).toFixed(1)+' KB':'—'}</td></tr>
        <tr><td>Min free heap</td><td>${st.minheap!=null?(st.minheap/1024).toFixed(1)+' KB':'—'}</td></tr>
        <tr><td>WiFi RSSI</td><td>${st.rssi} dBm</td></tr></table>
        <p style="color:var(--text2);font-size:.78rem;margin:.4rem 0 0">If uptime keeps resetting the device is rebooting; a steadily falling free-heap points to a leak.</p></div>

      <div class="dash-box compact"><h3>Ports / baud ${msg?html`<span style="float:right;font-weight:400;color:var(--green)">${msg}</span>`:''}</h3>
        <label>GPS baud</label><select id=sgb>${GBAUDS.map(b=>html`<option selected=${b==st.gpsbaud}>${b}</option>`)}</select>
        <label>LoRa baud</label><select id=slb>${LBAUDS.map(b=>html`<option selected=${b==st.lorabaud}>${b}</option>`)}</select>
        <label>GPS pins TX / RX</label><div style="display:grid;grid-template-columns:1fr 1fr;gap:8px"><input id=sgtx type=number value=${st.gtx} style="width:100%;min-width:0"/><input id=sgrx type=number value=${st.grx} style="width:100%;min-width:0"/></div>
        <label>LoRa pins TX / RX</label><div style="display:grid;grid-template-columns:1fr 1fr;gap:8px"><input id=sltx type=number value=${st.ltx} style="width:100%;min-width:0"/><input id=slrx type=number value=${st.lrx} style="width:100%;min-width:0"/></div>
        <label>LoRa E220 mode pins M0 / M1 / AUX (GPIO; AUX=-1 disables)</label><div style="display:grid;grid-template-columns:1fr 1fr 1fr;gap:8px"><input id=sm0 type=number value=${st.em0} style="width:100%;min-width:0"/><input id=sm1 type=number value=${st.em1} style="width:100%;min-width:0"/><input id=saux type=number value=${st.eaux} style="width:100%;min-width:0"/></div>
        <button onClick=${ports} style="margin-top:.6rem">Apply</button></div>

      <div class="dash-box compact"><h3>WiFi</h3>
        <label>Station SSID</label><input value=${w.ss===undefined?st.sta:w.ss} onInput=${e=>sv('ss',e.target.value)}/>
        <label>Station password</label><input type=password onInput=${e=>sv('sp',e.target.value)}/>
        <label>AP SSID</label><input value=${w.as===undefined?st.ap:w.as} onInput=${e=>sv('as',e.target.value)}/>
        <label>AP password</label><input type=password onInput=${e=>sv('ap',e.target.value)}/>
        <button onClick=${wifi} style="margin-top:.6rem">Save & reboot</button></div>

      <div class="dash-box compact"><h3>Firmware</h3>
        <form method=POST action=/update enctype=multipart/form-data>
          <input type=file name=fw accept=.bin /><button type=submit>Upload & flash</button></form>
        <p style="color:var(--text2);font-size:.8rem">Or OTA: <span style="font-family:var(--mono)">pio run -e rtk1010_web_ota -t upload</span></p>
        <button onClick=${() => { if(confirm('Reboot?')) fetch('/reboot'); }} style="margin-top:.4rem">Reboot</button></div>
    </div></div>`;
}

// ---- App shell ----
const TABS = [['dash','Dashboard'],['gps','RTK-1010'],['lora','LoRa'],['con','Consoles'],['sys','System']];

// Stroke icons (feather-style) — shown beside each tab label, and alone when the
// navbar collapses to its 60px rail in responsive mode (CSS hides .navbar-text).
const ICONS = {
  dash: '<rect x="3" y="3" width="7" height="7" rx="1"/><rect x="14" y="3" width="7" height="7" rx="1"/><rect x="3" y="14" width="7" height="7" rx="1"/><rect x="14" y="14" width="7" height="7" rx="1"/>',
  gps:  '<polygon points="3 11 22 2 13 21 11 13 3 11"/>',
  lora: '<circle cx="12" cy="12" r="2"/><path d="M16.24 7.76a6 6 0 0 1 0 8.49m-8.48-.01a6 6 0 0 1 0-8.49m11.31-2.82a10 10 0 0 1 0 14.14m-14.14 0a10 10 0 0 1 0-14.14"/>',
  con:  '<polyline points="4 17 10 11 4 5"/><line x1="12" y1="19" x2="20" y2="19"/>',
  sys:  '<circle cx="12" cy="12" r="3"/><path d="M19.4 15a1.65 1.65 0 0 0 .33 1.82l.06.06a2 2 0 1 1-2.83 2.83l-.06-.06a1.65 1.65 0 0 0-1.82-.33 1.65 1.65 0 0 0-1 1.51V21a2 2 0 0 1-4 0v-.09A1.65 1.65 0 0 0 9 19.4a1.65 1.65 0 0 0-1.82.33l-.06.06a2 2 0 1 1-2.83-2.83l.06-.06a1.65 1.65 0 0 0 .33-1.82 1.65 1.65 0 0 0-1.51-1H3a2 2 0 0 1 0-4h.09A1.65 1.65 0 0 0 4.6 9a1.65 1.65 0 0 0-.33-1.82l-.06-.06a2 2 0 1 1 2.83-2.83l.06.06a1.65 1.65 0 0 0 1.82.33H9a1.65 1.65 0 0 0 1-1.51V3a2 2 0 0 1 4 0v.09a1.65 1.65 0 0 0 1 1.51 1.65 1.65 0 0 0 1.82-.33l.06-.06a2 2 0 1 1 2.83 2.83l-.06.06a1.65 1.65 0 0 0-.33 1.82V9a1.65 1.65 0 0 0 1.51 1H21a2 2 0 0 1 0 4h-.09a1.65 1.65 0 0 0-1.51 1z"/>',
};
const svgIcon = (p) => '<svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">' + p + '</svg>';

// Logo: concentric RF arcs around a fix point inside a cyan→green ring — reads as
// "wireless positioning hub" (GPS + LoRa + WiFi). Scales to the 40/30px logo box.
const LOGO = '<svg viewBox="0 0 48 48" fill="none">'
  + '<defs><linearGradient id="rtklogo" x1="0" y1="0" x2="1" y2="1"><stop offset="0%" stop-color="#4cc9f0"/><stop offset="100%" stop-color="#54e6a4"/></linearGradient></defs>'
  + '<circle cx="24" cy="24" r="21" fill="none" stroke="url(#rtklogo)" stroke-width="2.6"/>'
  + '<circle cx="24" cy="24" r="3" fill="#4cc9f0"/>'
  + '<g fill="none" stroke-linecap="round" stroke-width="2.4">'
  + '<path d="M30.4 17.6a9 9 0 0 1 0 12.8" stroke="#4cc9f0"/><path d="M17.6 30.4a9 9 0 0 1 0-12.8" stroke="#4cc9f0"/>'
  + '<path d="M34.6 13.4a15 15 0 0 1 0 21.2" stroke="#54e6a4" opacity=".55"/><path d="M13.4 34.6a15 15 0 0 1 0-21.2" stroke="#54e6a4" opacity=".55"/>'
  + '</g></svg>';

// Sidebar status icons (coloured by live state, kept visible on the collapsed rail).
const SICON = {
  gps:  ICONS.gps,
  rtcm: ICONS.lora,
  usb:  '<path d="M10 13a5 5 0 0 0 7.54.54l3-3a5 5 0 0 0-7.07-7.07l-1.72 1.71"/><path d="M14 11a5 5 0 0 0-7.54-.54l-3 3a5 5 0 0 0 7.07 7.07l1.71-1.71"/>',
  tcp:  '<rect x="2" y="2" width="20" height="8" rx="2"/><rect x="2" y="14" width="20" height="8" rx="2"/><line x1="6" y1="6" x2="6.01" y2="6"/><line x1="6" y1="18" x2="6.01" y2="18"/>',
  wifi: '<path d="M5 12.55a11 11 0 0 1 14.08 0"/><path d="M1.42 9a16 16 0 0 1 21.16 0"/><path d="M8.53 16.11a6 6 0 0 1 6.95 0"/><line x1="12" y1="20" x2="12.01" y2="20"/>',
};

function App() {
  const [tab, setTab] = useState('dash');
  const [st, setSt] = useState(null);
  const [hist, setHist] = useState([]);
  const [sats, setSats] = useState([]);
  const [posHist, setPosHist] = useState([]);
  const prev = useRef(null), pt = useRef(0);

  useEffect(() => {
    let stop = false;
    const poll = async () => {
      try {
        const s = await gj('/status');
        const now = Date.now();
        if (prev.current && now > pt.current) {
          const dt = (now - pt.current) / 1000;
          const bps = (a, b) => Math.max(0, Math.round((s[a] + s[b] - prev.current[a] - prev.current[b]) / dt));
          setHist(h => [...h.slice(-59), {
            gpsSats: s.sats, rtcmSats: s.rtcmsats, rssi: s.rssi, fix: s.fix, hdop: s.hdop,
            gpsBps: bps('gpsrx','gpstx'), loraBps: bps('lorarx','loratx'), usbBps: bps('cdcrx','cdctx')
          }]);
        }
        prev.current = s; pt.current = now;
        setSt(s);
        if (s.fix > 0 && s.lat && s.lon) setPosHist(p => [...p.slice(-499), { lat: s.lat, lon: s.lon }]);
        gj('/sats').then(d => setSats(d.sats || [])).catch(() => {});
      } catch (e) {}
    };
    poll();
    const id = setInterval(poll, 1000);
    return () => { stop = true; clearInterval(id); };
  }, []);

  // Sidebar status badges — always visible regardless of tab. In the collapsed
  // (responsive) rail only the coloured dot shows, centred like the tab icons.
  const last = hist.length ? hist[hist.length - 1] : {};
  const lB = last.loraBps || 0;
  const MUTE = 'var(--text3)';
  const fixCol = st ? (st.fix >= 4 ? C.grn : st.fix > 0 ? C.acc : MUTE) : MUTE;
  const rtcmAct = !!(st && (lB > 0 || st.rtcmsats > 0));
  const statRow = (color, iconKey, label, sub) => html`<div class="tablink" style="cursor:default;pointer-events:none;padding:6px 14px;margin:0 10px">
    <span class="tab-icon" style=${'color:' + color} dangerouslySetInnerHTML=${{ __html: svgIcon(SICON[iconKey]) }}></span>
    <span class="navbar-text" style="line-height:1.12"><b style="font-weight:600">${label}</b>${sub!=null?html`<br><span style="color:var(--text3);font-size:.7rem">${sub}</span>`:''}</span></div>`;

  return html`
    <div id="navbar">
      <div id="logo" onClick=${() => setTab('dash')} style="cursor:pointer" dangerouslySetInnerHTML=${{ __html: LOGO + '<div class="navbar-text" style="text-align:center;font-weight:700;font-size:.8rem;color:var(--accent);margin-top:.45rem;line-height:1.15">RTK1010<br><span style="color:var(--text3);font-weight:500;font-size:.7rem">Router</span></div>' }}></div>
      ${TABS.map(([id, name]) => html`<div class=${'tablink' + (tab===id?' active':'')} onClick=${() => setTab(id)} title=${name}>
        <span class="tab-icon" dangerouslySetInnerHTML=${{ __html: svgIcon(ICONS[id]) }}></span>
        <span class="navbar-text">${name}</span></div>`)}
      <div style="margin-top:auto">
        <div style="height:1px;background:var(--border);margin:6px 12px"></div>
        ${statRow(fixCol, 'gps', 'GPS', st ? fixLabel(st) + ' · ' + st.sats + ' sats' : '…')}
        ${statRow(rtcmAct ? C.grn : MUTE, 'rtcm', 'RTCM', st ? (rtcmAct ? st.rtcmsats + ' sats / ' + st.rtcmcons + ' cons' : 'no data') : '…')}
        ${statRow(st && st.cdcconn ? C.grn : MUTE, 'usb', 'USB-CDC', st ? (st.cdcconn ? 'DTR' : 'no DTR') : '…')}
        ${statRow(st && st.tcp ? C.grn : MUTE, 'tcp', 'TCP :10110', st ? (st.tcp ? 'connected' : 'idle') : '…')}
        ${statRow(st ? C.grn : C.amb, 'wifi', st ? st.staip : 'connecting…', st ? ('RSSI ' + st.rssi + ' dBm') : null)}
        <div style="height:8px"></div>
      </div>
    </div>
    <div id="content-wrapper"><div id="content-wrapper-inner">
      ${tab==='dash' && html`<${Dashboard} st=${st} hist=${hist} sats=${sats} posHist=${posHist} onResetPos=${() => setPosHist([])} />`}
      ${tab==='gps'  && html`<${GpsTab} st=${st} />`}
      ${tab==='lora' && html`<${LoraTab} st=${st} />`}
      ${tab==='con'  && html`<${Consoles} />`}
      ${tab==='sys'  && html`<${SystemTab} st=${st} />`}
    </div></div>`;
}

render(html`<${App} />`, document.body);
