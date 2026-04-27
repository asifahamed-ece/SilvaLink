/* ============================================
   SilvaLink Admin Dashboard — WebSocket Client
   Real-time communication with Admin ESP32
   ============================================ */

(function () {
  'use strict';

  // ---- State ----
  const state = {
    ws: null,
    connected: false,
    nodes: {},          // nodeId -> { temp, hum, rssi, lastSeen, status }
    activeSOS: null,    // Current active SOS alert
    logs: [],           // Activity log entries (max 50)
    packetCount: 0,
    startTime: Date.now(),
    reconnectDelay: 1000,
    maxReconnectDelay: 16000,
  };

  const MAX_LOG_ENTRIES = 50;

  // ---- DOM References ----
  const $ = (sel) => document.querySelector(sel);
  const $$ = (sel) => document.querySelectorAll(sel);

  // ---- WebSocket ----
  function connectWebSocket() {
    const host = window.location.hostname || '192.168.4.1';
    const wsUrl = `ws://${host}/ws`;

    try {
      state.ws = new WebSocket(wsUrl);
    } catch (e) {
      scheduleReconnect();
      return;
    }

    state.ws.onopen = function () {
      state.connected = true;
      state.reconnectDelay = 1000;
      updateConnectionUI(true);
      addLog('system', 'Dashboard connected to Admin node');
    };

    state.ws.onclose = function () {
      state.connected = false;
      updateConnectionUI(false);
      addLog('system', 'Connection lost. Reconnecting...');
      scheduleReconnect();
    };

    state.ws.onerror = function () {
      state.connected = false;
      updateConnectionUI(false);
    };

    state.ws.onmessage = function (event) {
      try {
        const data = JSON.parse(event.data);
        handleMessage(data);
      } catch (e) {
        console.warn('Invalid message:', event.data);
      }
    };
  }

  function scheduleReconnect() {
    setTimeout(function () {
      connectWebSocket();
      state.reconnectDelay = Math.min(state.reconnectDelay * 2, state.maxReconnectDelay);
    }, state.reconnectDelay);
  }

  function sendMessage(data) {
    if (state.ws && state.ws.readyState === WebSocket.OPEN) {
      state.ws.send(JSON.stringify(data));
    }
  }

  // ---- Message Handler ----
  function handleMessage(data) {
    state.packetCount++;

    switch (data.type) {
      case 'env':
        handleEnvData(data);
        break;
      case 'sos':
        handleSOS(data);
        break;
      case 'ack':
        handleAck(data);
        break;
      case 'status':
        handleSystemStatus(data);
        break;
      default:
        addLog('system', 'Unknown packet type: ' + data.type);
    }

    updateSystemBar();
  }

  function handleEnvData(data) {
    const nodeId = data.node || 'A';
    const now = Date.now();

    if (!state.nodes[nodeId]) {
      state.nodes[nodeId] = { temp: 0, hum: 0, rssi: 0, lastSeen: now, status: 'online' };
    }

    const node = state.nodes[nodeId];
    node.temp = parseFloat(data.temp) || 0;
    node.hum = parseFloat(data.hum) || 0;
    node.rssi = parseInt(data.rssi) || 0;
    node.lastSeen = now;
    node.status = 'online';

    updateStatsCards();
    renderNodeCards();
    addLog('env', `<strong>Node ${nodeId}</strong> — ${node.temp.toFixed(1)}°C, ${node.hum.toFixed(1)}% RH, RSSI ${node.rssi}`);
  }

  function handleSOS(data) {
    const msg = data.msg || '';
    // Parse SOS message: "!!! SOS ALERT !!! Type: MEDICAL | Msg: Help needed"
    // Or structured: node, sosType, customMsg
    let sosType = data.sosType || 'UNKNOWN';
    let customMsg = data.customMsg || '';
    let nodeId = data.node || '?';

    // Fallback: parse from raw message string
    if (msg) {
      const typeMatch = msg.match(/Type:\s*(\w+)/i);
      if (typeMatch) sosType = typeMatch[1];
      const msgMatch = msg.match(/Msg:\s*(.+)/i);
      if (msgMatch) customMsg = msgMatch[1].trim();
      // Try to extract node from start
      const nodeMatch = msg.match(/Node[:\s]*(\w)/i);
      if (nodeMatch) nodeId = nodeMatch[1];
    }

    state.activeSOS = {
      nodeId: nodeId,
      type: sosType,
      message: customMsg,
      time: new Date(),
      raw: msg
    };

    // Mark node as SOS
    if (state.nodes[nodeId]) {
      state.nodes[nodeId].status = 'sos';
    }

    showSOSPanel();
    renderNodeCards();
    addLog('sos', `🚨 <strong>SOS ${sosType}</strong> from Node ${nodeId}${customMsg ? ': ' + customMsg : ''}`);
    playSOSAlert();
  }

  function handleAck(data) {
    addLog('ack', `ACK sent to Node ${data.node || '?'}`);
  }

  function handleSystemStatus(data) {
    // Optional system health updates
    if (data.uptime) {
      const uptimeEl = $('#sys-uptime');
      if (uptimeEl) uptimeEl.textContent = formatUptime(data.uptime);
    }
  }

  // ---- SOS Alert UI ----
  function showSOSPanel() {
    const panel = $('#sos-panel');
    if (!panel || !state.activeSOS) return;

    panel.style.display = 'block';
    panel.classList.add('active');
    $('#sos-type').textContent = `Type: ${state.activeSOS.type} — Node ${state.activeSOS.nodeId}`;
    $('#sos-msg').textContent = state.activeSOS.message || 'No additional message';
    $('#sos-time').textContent = formatTime(state.activeSOS.time);
  }

  function dismissSOS() {
    const panel = $('#sos-panel');
    if (panel) {
      panel.style.display = 'none';
      panel.classList.remove('active');
    }

    // Send ACK to admin node → relayed via LoRa
    if (state.activeSOS) {
      sendMessage({ type: 'ack', node: state.activeSOS.nodeId });

      // Reset node status
      if (state.nodes[state.activeSOS.nodeId]) {
        state.nodes[state.activeSOS.nodeId].status = 'online';
      }
      renderNodeCards();
    }

    state.activeSOS = null;
    addLog('ack', 'SOS acknowledged and ACK sent');
  }

  // ---- Stats Cards ----
  function updateStatsCards() {
    // Aggregate from all nodes
    const nodeIds = Object.keys(state.nodes);
    const activeNodes = nodeIds.filter(function (id) {
      return Date.now() - state.nodes[id].lastSeen < 30000;
    });

    // Show latest / average values (latest from most recent node)
    let latestNode = null;
    let latestTime = 0;
    nodeIds.forEach(function (id) {
      if (state.nodes[id].lastSeen > latestTime) {
        latestTime = state.nodes[id].lastSeen;
        latestNode = state.nodes[id];
      }
    });

    if (latestNode) {
      animateValue($('#stat-temp'), latestNode.temp.toFixed(1));
      animateValue($('#stat-hum'), latestNode.hum.toFixed(1));
      animateValue($('#stat-rssi'), latestNode.rssi);
    }

    animateValue($('#stat-nodes'), activeNodes.length);

    // Update sub-text
    const tempSub = $('#stat-temp-sub');
    if (tempSub && latestNode) {
      tempSub.textContent = latestNode.temp > 35 ? '⚠ High temperature' :
        latestNode.temp < 5 ? '❄ Low temperature' : 'Normal range';
    }
  }

  function animateValue(el, newVal) {
    if (!el) return;
    el.textContent = newVal;
  }

  // ---- Node Cards ----
  function renderNodeCards() {
    const container = $('#nodes-container');
    if (!container) return;

    const nodeIds = Object.keys(state.nodes);

    if (nodeIds.length === 0) {
      container.innerHTML = '<div class="log-empty">Waiting for node data...</div>';
      return;
    }

    let html = '';
    nodeIds.forEach(function (id) {
      const node = state.nodes[id];
      const isOnline = Date.now() - node.lastSeen < 30000;
      const status = node.status === 'sos' ? 'sos' : (isOnline ? 'online' : 'offline');
      const statusLabel = status === 'sos' ? 'SOS ACTIVE' : (isOnline ? 'ONLINE' : 'OFFLINE');
      const rssiLevel = getRSSILevel(node.rssi);
      const tempPercent = Math.min(Math.max((node.temp / 50) * 100, 0), 100);
      const humPercent = Math.min(Math.max(node.hum, 0), 100);

      html += `
        <div class="node-card ${status === 'sos' ? 'sos-active' : ''}">
          <div class="node-header">
            <div class="node-id">
              <div class="node-avatar">${id}</div>
              <div>
                <div class="node-name">Node ${id}</div>
                <div class="node-role">Trekker Sensor</div>
              </div>
            </div>
            <span class="node-status ${status}">${statusLabel}</span>
          </div>
          <div class="node-metrics">
            <div class="metric">
              <div class="metric-label">Temperature</div>
              <div class="metric-value temp-val">${node.temp.toFixed(1)}<span class="stat-unit">°C</span></div>
              <div class="metric-bar"><div class="metric-bar-fill temp-bar" style="width:${tempPercent}%"></div></div>
            </div>
            <div class="metric">
              <div class="metric-label">Humidity</div>
              <div class="metric-value hum-val">${node.hum.toFixed(1)}<span class="stat-unit">%</span></div>
              <div class="metric-bar"><div class="metric-bar-fill hum-bar" style="width:${humPercent}%"></div></div>
            </div>
          </div>
          <div class="node-footer">
            <div class="rssi-display">
              <div class="rssi-bars">
                <div class="rssi-bar ${rssiLevel >= 1 ? 'active' : ''}"></div>
                <div class="rssi-bar ${rssiLevel >= 2 ? 'active' : ''}"></div>
                <div class="rssi-bar ${rssiLevel >= 3 ? 'active' : ''}"></div>
                <div class="rssi-bar ${rssiLevel >= 4 ? 'active' : ''}"></div>
              </div>
              ${node.rssi} dBm
            </div>
            <div class="last-seen">${formatTimeSince(node.lastSeen)}</div>
          </div>
        </div>`;
    });

    container.innerHTML = html;
  }

  // ---- Activity Log ----
  function addLog(type, text) {
    const entry = {
      type: type,
      text: text,
      time: new Date()
    };

    state.logs.unshift(entry);
    if (state.logs.length > MAX_LOG_ENTRIES) {
      state.logs.pop();
    }

    renderLog();
  }

  function renderLog() {
    const list = $('#log-list');
    if (!list) return;

    if (state.logs.length === 0) {
      list.innerHTML = '<div class="log-empty">No activity yet. Waiting for LoRa packets...</div>';
      return;
    }

    let html = '';
    state.logs.forEach(function (entry) {
      html += `
        <div class="log-entry">
          <div class="log-dot ${entry.type}"></div>
          <div class="log-content">
            <div class="log-text">${entry.text}</div>
            <div class="log-time">${formatTime(entry.time)}</div>
          </div>
        </div>`;
    });

    list.innerHTML = html;
  }

  // ---- Connection UI ----
  function updateConnectionUI(connected) {
    const badge = $('#conn-badge');
    if (!badge) return;

    if (connected) {
      badge.className = 'conn-badge online';
      badge.querySelector('.conn-label').textContent = 'LIVE';
    } else {
      badge.className = 'conn-badge offline';
      badge.querySelector('.conn-label').textContent = 'OFFLINE';
    }
  }

  // ---- System Bar ----
  function updateSystemBar() {
    const uptime = $('#sys-uptime');
    if (uptime) uptime.textContent = formatUptime(Date.now() - state.startTime);

    const packets = $('#sys-packets');
    if (packets) packets.textContent = state.packetCount;

    const clients = $('#sys-clients');
    // Client count is managed server-side; we know at least 1 (us)
  }

  // ---- SOS Audio Alert ----
  function playSOSAlert() {
    try {
      const ctx = new (window.AudioContext || window.webkitAudioContext)();
      const now = ctx.currentTime;

      // Three beeps
      for (let i = 0; i < 3; i++) {
        const osc = ctx.createOscillator();
        const gain = ctx.createGain();
        osc.connect(gain);
        gain.connect(ctx.destination);
        osc.frequency.value = 880;
        osc.type = 'square';
        gain.gain.value = 0.15;
        osc.start(now + i * 0.3);
        osc.stop(now + i * 0.3 + 0.15);
      }
    } catch (e) {
      // Audio not available
    }
  }

  // ---- Utility Functions ----
  function getRSSILevel(rssi) {
    if (rssi >= -50) return 4;
    if (rssi >= -70) return 3;
    if (rssi >= -85) return 2;
    if (rssi >= -100) return 1;
    return 0;
  }

  function formatTime(date) {
    if (!(date instanceof Date)) date = new Date(date);
    const h = date.getHours().toString().padStart(2, '0');
    const m = date.getMinutes().toString().padStart(2, '0');
    const s = date.getSeconds().toString().padStart(2, '0');
    return h + ':' + m + ':' + s;
  }

  function formatTimeSince(timestamp) {
    const diff = Math.floor((Date.now() - timestamp) / 1000);
    if (diff < 5) return 'Just now';
    if (diff < 60) return diff + 's ago';
    if (diff < 3600) return Math.floor(diff / 60) + 'm ago';
    return Math.floor(diff / 3600) + 'h ago';
  }

  function formatUptime(ms) {
    const s = Math.floor(ms / 1000);
    const m = Math.floor(s / 60);
    const h = Math.floor(m / 60);
    if (h > 0) return h + 'h ' + (m % 60) + 'm';
    if (m > 0) return m + 'm ' + (s % 60) + 's';
    return s + 's';
  }

  // ---- Periodic Updates ----
  setInterval(function () {
    // Update "last seen" times and check node timeouts
    const nodeIds = Object.keys(state.nodes);
    nodeIds.forEach(function (id) {
      if (Date.now() - state.nodes[id].lastSeen > 30000 && state.nodes[id].status !== 'sos') {
        state.nodes[id].status = 'offline';
      }
    });

    renderNodeCards();
    updateSystemBar();
  }, 5000);

  // ---- Init ----
  document.addEventListener('DOMContentLoaded', function () {
    // Bind SOS Acknowledge button
    const ackBtn = $('#sos-ack-btn');
    if (ackBtn) ackBtn.addEventListener('click', dismissSOS);

    // Start WebSocket
    connectWebSocket();

    // Initial render
    renderNodeCards();
    renderLog();
    updateSystemBar();
  });

  // Expose dismissSOS for inline onclick if needed
  window.silvalink = { dismissSOS: dismissSOS };

})();
