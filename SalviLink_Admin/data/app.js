/* ============================================
SilvaLink Admin Dashboard — WebSocket Client
Real-time communication + Smooth Charts
============================================ */
(function () {
'use strict';

const state = {
  ws: null, connected: false, nodes: {}, activeSOS: null,
  logs: [], packetCount: 0, startTime: Date.now(),
  reconnectDelay: 1000, maxReconnectDelay: 16000,
  lastRender: 0, animationFrame: null
};
const MAX_LOG_ENTRIES = 50;
const $ = (sel) => document.querySelector(sel);

function connectWebSocket() {
  const host = window.location.hostname || '192.168.4.1';
  try { state.ws = new WebSocket(`ws://${host}/ws`); } catch (e) { scheduleReconnect(); return; }
  state.ws.onopen = () => { state.connected = true; state.reconnectDelay = 1000; updateConnectionUI(true); addLog('system', 'Connected to Admin'); };
  state.ws.onclose = () => { state.connected = false; updateConnectionUI(false); addLog('system', 'Disconnected. Reconnecting...'); scheduleReconnect(); };
  state.ws.onerror = () => { state.connected = false; updateConnectionUI(false); };
  state.ws.onmessage = (e) => { try { handleMessage(JSON.parse(e.data)); } catch(err) {} };
}
function scheduleReconnect() { setTimeout(() => { connectWebSocket(); state.reconnectDelay = Math.min(state.reconnectDelay * 2, state.maxReconnectDelay); }, state.reconnectDelay); }
function sendMessage(d) { if (state.ws?.readyState === 1) state.ws.send(JSON.stringify(d)); }

function handleMessage(data) {
  state.packetCount++;
  switch (data.type) {
    case 'env': handleEnvData(data); break;
    case 'sos': handleSOS(data); break;
    case 'ack': handleAck(data); break;
  }
  updateSystemBar(); requestRender();
}

function handleEnvData(data) {
  if (!data.node) return;
  const id = String(data.node);
  const now = Date.now();
  if (!state.nodes[id]) {
    state.nodes[id] = { temp: 0, hum: 0, rssi: 0, lastSeen: now, status: 'online', history: { temp: [], hum: [] } };
  }
  const node = state.nodes[id];
  node.temp = parseFloat(data.temp) || 0;
  node.hum  = parseFloat(data.hum) || 0;
  node.rssi = parseInt(data.rssi) || 0;
  node.lastSeen = now;
  node.status = 'online';
  node.history.temp.push(node.temp); if (node.history.temp.length > 30) node.history.temp.shift();
  node.history.hum.push(node.hum);    if (node.history.hum.length > 30) node.history.hum.shift();
  updateStatsCards();
  addLog('env', `<strong>Node ${id}</strong> — ${node.temp.toFixed(1)}°C, ${node.hum.toFixed(1)}%`);
}

function handleSOS(data) {
  const id = String(data.node || '?');
  state.activeSOS = { nodeId: id, type: data.sosType || 'UNKNOWN', message: data.customMsg || '', time: new Date() };
  if (state.nodes[id]) state.nodes[id].status = 'sos';
  showSOS(); requestRender(); addLog('sos', `🚨 <strong>SOS ${state.activeSOS.type}</strong> from Node ${id}`); playBeep();
}
function handleAck(d) { addLog('ack', `ACK sent to Node ${d.node || '?'}`); }

function showSOS() {
  const p = $('#sos-panel'); if (!p || !state.activeSOS) return;
  p.style.display = 'block'; setTimeout(() => p.classList.add('active'), 10);
  $('#sos-type').textContent = `Type: ${state.activeSOS.type} — Node ${state.activeSOS.nodeId}`;
  $('#sos-msg').textContent = state.activeSOS.message;
  $('#sos-time').textContent = formatTime(state.activeSOS.time);
}
function dismissSOS() {
  const p = $('#sos-panel'); if (p) { p.classList.remove('active'); setTimeout(() => p.style.display = 'none', 300); }
  if (state.activeSOS) {
    sendMessage({ type: 'ack', node: state.activeSOS.nodeId });
    if (state.nodes[state.activeSOS.nodeId]) state.nodes[state.activeSOS.nodeId].status = 'online';
    requestRender();
  }
  state.activeSOS = null; addLog('ack', 'SOS acknowledged');
}

function requestRender() {
  if (state.animationFrame) return;
  state.animationFrame = requestAnimationFrame(() => {
    const now = performance.now();
    if (now - state.lastRender > 100) { renderNodeCards(); state.lastRender = now; }
    state.animationFrame = null;
  });
}

function updateStatsCards() {
  const active = Object.values(state.nodes).filter(n => Date.now() - n.lastSeen < 30000);
  let avg = { temp: 0, hum: 0, rssi: 0 };
  if (active.length) {
    active.forEach(n => { avg.temp += n.temp; avg.hum += n.hum; avg.rssi += n.rssi; });
    avg.temp /= active.length; avg.hum /= active.length; avg.rssi /= active.length;
  }
  animateValue($('#stat-temp'), avg.temp.toFixed(1));
  animateValue($('#stat-hum'), avg.hum.toFixed(1));
  animateValue($('#stat-rssi'), Math.round(avg.rssi));
  animateValue($('#stat-nodes'), active.length);
  const sub = $('#stat-temp-sub');
  if (sub) sub.textContent = avg.temp > 35 ? '⚠ High' : avg.temp < 5 ? '❄ Low' : 'Normal range';
}
function animateValue(el, newVal) { if (el && el.textContent !== newVal) el.textContent = newVal; }

// ---- 📊 SMOOTH MINI-CHART GENERATOR ----
function renderChart(data, color, width, height, unit) {
  if (!data || data.length < 2) return `<svg class="mini-chart" width="100%" height="${height}"></svg>`;
  
  const pad = { top: 6, right: 8, bottom: 18, left: 32 };
  const w = width - pad.left - pad.right;
  const h = height - pad.top - pad.bottom;
  const min = Math.min(...data);
  const max = Math.max(...data);
  const range = max - min || 1;

  const pts = data.map((v, i) => `${pad.left + (i / (data.length - 1)) * w},${pad.top + h - ((v - min) / range) * h}`).join(' ');
  
  // Y-axis ticks (3 levels)
  const yLabels = [min, min + range/2, max].map(v => 
    `<text x="${pad.left-4}" y="${pad.top + h - ((v-min)/range)*h + 3}" class="chart-tick">${v.toFixed(1)}</text>`
  ).join('');

  return `
    <svg class="mini-chart" viewBox="0 0 ${width} ${height}" preserveAspectRatio="none">
      <defs>
        <linearGradient id="g-${color.replace('#','')}" x1="0" y1="0" x2="0" y2="1">
          <stop offset="0%" stop-color="${color}" stop-opacity="0.3"/>
          <stop offset="100%" stop-color="${color}" stop-opacity="0"/>
        </linearGradient>
      </defs>
      <!-- Grid -->
      <line x1="${pad.left}" y1="${pad.top}" x2="${pad.left+w}" y2="${pad.top}" stroke="rgba(255,255,255,0.04)" stroke-dasharray="2,3"/>
      <line x1="${pad.left}" y1="${pad.top+h/2}" x2="${pad.left+w}" y2="${pad.top+h/2}" stroke="rgba(255,255,255,0.04)" stroke-dasharray="2,3"/>
      <line x1="${pad.left}" y1="${pad.top+h}" x2="${pad.left+w}" y2="${pad.top+h}" stroke="rgba(255,255,255,0.08)"/>
      
      <!-- Area & Smooth Line -->
      <path d="M ${pts} L ${pad.left+w},${pad.top+h} L ${pad.left},${pad.top+h} Z" fill="url(#g-${color.replace('#','')})"/>
      <path d="M ${pts}" fill="none" stroke="${color}" stroke-width="2.5" stroke-linejoin="round" stroke-linecap="round" class="chart-line"/>
      
      <!-- Axes -->
      ${yLabels}
      <line x1="${pad.left}" y1="${pad.top}" x2="${pad.left}" y2="${pad.top+h}" stroke="rgba(255,255,255,0.1)"/>
      <text x="${pad.left + w/2}" y="${height - 2}" class="chart-axis-label">Time →</text>
    </svg>`;
}

function renderNodeCards() {
  const c = $('#nodes-container'); if (!c) return;
  const ids = Object.keys(state.nodes);
  if (!ids.length) { c.innerHTML = '<div class="log-empty">Waiting for node data...</div>'; return; }
  
  let html = '';
  ids.forEach(id => {
    const n = state.nodes[id];
    const online = Date.now() - n.lastSeen < 30000;
    const status = n.status === 'sos' ? 'sos' : (online ? 'online' : 'offline');
    const rssiLvl = n.rssi >= -50 ? 4 : n.rssi >= -70 ? 3 : n.rssi >= -85 ? 2 : 1;
    const dim = status === 'offline' ? 'opacity:0.55;' : '';
    
    html += `
      <div class="node-card ${status === 'sos' ? 'sos-active' : ''}" style="${dim}">
        <div class="node-header">
          <div class="node-id"><div class="node-avatar">${id}</div>
          <div><div class="node-name">Node ${id}</div><div class="node-role">Trekker Sensor</div></div></div>
          <span class="node-status ${status}">${status === 'sos' ? 'SOS ACTIVE' : (online ? 'ONLINE' : 'OFFLINE')}</span>
        </div>
        <div class="node-metrics">
          <div class="metric">
            <div class="metric-label">Temperature</div>
            <div class="metric-value temp-val">${n.temp.toFixed(1)}°C</div>
            ${renderChart(n.history.temp, '#f59e0b', 280, 60)}
          </div>
          <div class="metric">
            <div class="metric-label">Humidity</div>
            <div class="metric-value hum-val">${n.hum.toFixed(1)}%</div>
            ${renderChart(n.history.hum, '#22d3ee', 280, 60)}
          </div>
        </div>
        <div class="node-footer">
          <div class="rssi-display"><div class="rssi-bars">
            <div class="rssi-bar ${rssiLvl>=1?'active':''}"></div><div class="rssi-bar ${rssiLvl>=2?'active':''}"></div>
            <div class="rssi-bar ${rssiLvl>=3?'active':''}"></div><div class="rssi-bar ${rssiLvl>=4?'active':''}"></div>
          </div>${n.rssi} dBm</div>
          <div class="last-seen">${Math.floor((Date.now()-n.lastSeen)/1000)}s ago</div>
        </div>
      </div>`;
  });
  c.innerHTML = html;
}

function addLog(type, text) {
  state.logs.unshift({ type, text, time: new Date() });
  if (state.logs.length > MAX_LOG_ENTRIES) state.logs.pop();
  const l = $('#log-list'); if (!l) return;
  l.innerHTML = state.logs.map(e => `<div class="log-entry"><div class="log-dot ${e.type}"></div><div class="log-content"><div class="log-text">${e.text}</div><div class="log-time">${formatTime(e.time)}</div></div></div>`).join('');
}
function updateConnectionUI(on) { const b = $('#conn-badge'); if (b) { b.className = `conn-badge ${on?'online':'offline'}`; b.querySelector('.conn-label').textContent = on?'LIVE':'OFFLINE'; } }
function updateSystemBar() { const u = Date.now() - state.startTime; $('#sys-uptime').textContent = `${Math.floor(u/3600000)}h ${Math.floor(u%3600000/60000)}m`; $('#sys-packets').textContent = state.packetCount; }
function playBeep() { try { const c = new (AudioContext||webkitAudioContext)(); [0,0.3,0.6].forEach(d => { const o = c.createOscillator(), g = c.createGain(); o.connect(g); g.connect(c.destination); o.frequency.value=880; g.gain.value=0.15; o.start(c.currentTime+d); o.stop(c.currentTime+d+0.15); }); } catch(e){} }
function formatTime(d) { return d.toLocaleTimeString(); }

setInterval(() => {
  Object.keys(state.nodes).forEach(id => { if (Date.now()-state.nodes[id].lastSeen>30000 && state.nodes[id].status!=='sos') state.nodes[id].status='offline'; });
  renderNodeCards(); updateSystemBar();
}, 1000);

document.addEventListener('DOMContentLoaded', () => {
  $('#sos-ack-btn')?.addEventListener('click', dismissSOS);
  connectWebSocket(); renderNodeCards(); updateSystemBar();
});
window.silvalink = { dismissSOS };
})();