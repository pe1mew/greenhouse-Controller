'use strict';

// ── Auth state ───────────────────────────────────────────────────────────────
let g_role = null;  // 'farmer' | 'admin' | null

function setRole(role) {
  g_role = role;
  document.body.classList.toggle('is-farmer', role === 'farmer' || role === 'admin');
  document.body.classList.toggle('is-admin',  role === 'admin');

  const rb = document.getElementById('role-badge');
  if (rb) {
    rb.textContent = role ? role.toUpperCase() : '';
    rb.style.display = role ? 'inline-block' : 'none';
    rb.style.background = role === 'admin' ? '#0f3460' : '#1a5276';
  }
  if (role) {
    const ov = document.getElementById('login-overlay');
    if (ov) ov.style.display = 'none';
    loadConfig();
    loadHistory();
  }
}

// ── WebSocket ────────────────────────────────────────────────────────────────
let ws = null;
let wsInitialized = false;

function wsConnect() {
  const proto = location.protocol === 'https:' ? 'wss:' : 'ws:';
  ws = new WebSocket(proto + '//' + location.host + '/ws');

  ws.onopen = function () {
    setBadge('ws-badge', 'Online', 'online');
  };
  ws.onclose = function () {
    setBadge('ws-badge', 'Offline', 'offline');
    wsInitialized = false;
    setTimeout(wsConnect, 3000);
  };
  ws.onerror = function () { ws.close(); };
  ws.onmessage = function (evt) {
    try {
      const msg = JSON.parse(evt.data);
      if (msg.type === 'status') handleStatus(msg);
    } catch (_) {}
  };
}

// ── Status handler ───────────────────────────────────────────────────────────
const WIN_LABELS = { OPEN: 'OPEN', CLOSED: 'CLOSED',
                     MOVING_OPEN: 'MOVING', MOVING_CLOSE: 'MOVING', UNKNOWN: '?' };
const WIN_CLASS  = { OPEN: 'win-open', CLOSED: 'win-closed',
                     MOVING_OPEN: 'win-moving', MOVING_CLOSE: 'win-moving', UNKNOWN: 'win-unknown' };

function handleStatus(s) {
  if (s.temp_c    !== undefined) setText('st-temp',      s.temp_c.toFixed(1));
  if (s.temp_avg  !== undefined) setText('st-temp-avg',  s.temp_avg.toFixed(1));
  if (s.rh_pct    !== undefined) setText('st-rh',        s.rh_pct.toFixed(0));
  if (s.rh_avg    !== undefined) setText('st-rh-avg',    s.rh_avg.toFixed(0));
  if (s.wind_ms   !== undefined) setText('st-wind',      s.wind_ms.toFixed(1));
  if (s.wind_dir  !== undefined) setText('st-wind-dir',  s.wind_dir.toFixed(0));
  if (s.wind_avg  !== undefined) setText('st-wind-avg',  s.wind_avg.toFixed(1));
  if (s.wifi_rssi !== undefined) setText('st-wifi-rssi', s.wifi_rssi);
  if (s.wifi_ip)                 setText('st-wifi-ip',   s.wifi_ip);

  // Windows
  if (s.windows) {
    for (let i = 0; i < 3; i++) {
      const el = document.getElementById('st-win' + i);
      if (!el) continue;
      const st = s.windows[i] || 'UNKNOWN';
      el.textContent = WIN_LABELS[st] || st;
      el.className = WIN_CLASS[st] || '';
    }
  }

  // Mode
  if (s.mode) {
    const modeNames = {
      AUTOMATIC: 'Automatic', STANDBY: 'Standby',
      WIND_OVERRIDE: 'Wind override', MOTOR_ALARM: 'Motor alarm'
    };
    setText('st-mode', modeNames[s.mode] || s.mode);
  }

  // Day/night + sunrise/sunset
  if (s.is_daytime !== undefined) setText('st-daytime', s.is_daytime ? 'Daytime' : 'Night');
  if (s.sunrise_utc !== undefined) setText('st-sunrise', utcMinsToStr(s.sunrise_utc));
  if (s.sunset_utc  !== undefined) setText('st-sunset',  utcMinsToStr(s.sunset_utc));

  // Alarms
  if (s.eg1 !== undefined) {
    const flags = [];
    if (s.eg1 & 1)  flags.push('<span class="badge alarm">WIND</span>');
    if (s.eg1 & 4)  flags.push('<span class="badge warn">T/RH fault</span>');
    if (s.eg1 & 8)  flags.push('<span class="badge warn">Wind fault</span>');
    if (s.eg1 & 16) flags.push('<span class="badge warn">OTA active</span>');
    if (s.eg1 & 32) flags.push('<span class="badge alarm">MOTOR ALARM</span>');
    const el = document.getElementById('st-alarms');
    if (el) el.innerHTML = flags.length ? flags.join(' ') : '<span class="badge ok">OK</span>';
  }

  // Time / NTP
  if (s.time) setText('st-time', s.time.replace('T', ' '));
  const ntpEl = document.getElementById('st-ntp');
  if (ntpEl) {
    ntpEl.textContent = s.ntp_synced ? 'NTP synced' : 'NTP pending';
    ntpEl.className   = 'badge ' + (s.ntp_synced ? 'ntp-on' : 'ntp-off');
  }

  // Firmware version (first message only)
  if (!wsInitialized && s.fw_ver) setText('fw-ver', 'v' + s.fw_ver);
  wsInitialized = true;
}

function utcMinsToStr(mins) {
  if (mins < 0 || mins > 1440) return '—';
  const h = Math.floor(mins / 60);
  const m = mins % 60;
  return String(h).padStart(2,'0') + ':' + String(m).padStart(2,'0');
}

// ── Auth ─────────────────────────────────────────────────────────────────────
let g_login_role = 'farmer';

function selectRole(role) {
  g_login_role = role;
  document.getElementById('btn-role-farmer').classList.toggle('active', role === 'farmer');
  document.getElementById('btn-role-admin').classList.toggle('active',  role === 'admin');
  document.getElementById('login-pin').focus();
}

function doLogin() {
  const role = g_login_role;
  const pin  = document.getElementById('login-pin').value;
  post('/api/login', { role, pin })
    .then(r => {
      if (r && r.ok) {
        setRole(r.role);
        document.getElementById('login-err').textContent = '';
        document.getElementById('login-pin').value = '';
      } else {
        const remaining = (r && r.remaining) ? '  (' + r.remaining + ' attempts left)' : '';
        document.getElementById('login-err').textContent =
          (r && r.locked) ? 'Locked out. Try again later.' : 'Wrong PIN.' + remaining;
      }
    });
}

document.addEventListener('keydown', function(e) {
  if (e.key === 'Enter' && document.getElementById('login-overlay').style.display !== 'none') {
    doLogin();
  }
});

function doLogout() {
  post('/api/logout', {}).then(() => {
    setRole(null);
    wsInitialized = false;
    document.getElementById('login-overlay').style.display = 'flex';
  });
}

// ── Config load ──────────────────────────────────────────────────────────────
function loadConfig() {
  fetch('/api/config')
    .then(r => r.ok ? r.json() : null)
    .then(cfg => {
      if (!cfg) return;
      setVal('cfg-t-max-day',      cfg.t_max_day);
      setVal('cfg-t-min-day',      cfg.t_min_day);
      setVal('cfg-t-max-ngt',      cfg.t_max_ngt);
      setVal('cfg-t-min-ngt',      cfg.t_min_ngt);
      setVal('cfg-rh-max-day',     cfg.rh_max_day);
      setVal('cfg-rh-min-day',     cfg.rh_min_day);
      setVal('cfg-rh-max-ngt',     cfg.rh_max_ngt);
      setVal('cfg-rh-min-ngt',     cfg.rh_min_ngt);
      setVal('cfg-hyst-t',         cfg.hyst_t);
      setVal('cfg-hyst-rh',        cfg.hyst_rh);
      setVal('cfg-avg-win-t',      cfg.avg_win_t);
      setVal('cfg-avg-win-rh',     cfg.avg_win_rh);
      setVal('cfg-rh-ctrl-en',     String(cfg.rh_ctrl_en));
      setVal('cfg-v-max',          cfg.v_max);
      setVal('cfg-dir-excl-low',   cfg.dir_excl_low);
      setVal('cfg-dir-excl-high',  cfg.dir_excl_high);
      setVal('cfg-wind-prot-en',   String(cfg.wind_prot_en));
      setVal('cfg-travel-0',       cfg.travel_s && cfg.travel_s[0]);
      setVal('cfg-travel-1',       cfg.travel_s && cfg.travel_s[1]);
      setVal('cfg-travel-2',       cfg.travel_s && cfg.travel_s[2]);
      setVal('cfg-dwell-open-0',   cfg.dwell_open_min && cfg.dwell_open_min[0]);
      setVal('cfg-dwell-open-1',   cfg.dwell_open_min && cfg.dwell_open_min[1]);
      setVal('cfg-dwell-open-2',   cfg.dwell_open_min && cfg.dwell_open_min[2]);
      setVal('cfg-dwell-close-0',  cfg.dwell_close_min && cfg.dwell_close_min[0]);
      setVal('cfg-dwell-close-1',  cfg.dwell_close_min && cfg.dwell_close_min[1]);
      setVal('cfg-dwell-close-2',  cfg.dwell_close_min && cfg.dwell_close_min[2]);
      setVal('cfg-session-timeout', cfg.session_timeout_min);
      setVal('cfg-ap-timeout',     cfg.ap_timeout_min);
      setVal('cfg-poll-interval',  cfg.poll_interval_s);
      setVal('cfg-wifi-ssid',      cfg.wifi_ssid);
      setText('cfg-ap-ssid',       cfg.ap_ssid);
      setVal('cfg-tz',             cfg.tz_str);
      if (cfg.lat_deg !== undefined && cfg.lat_frac !== undefined) {
        setVal('cfg-lat', (cfg.lat_deg + cfg.lat_frac / 1000.0).toFixed(3));
      }
      if (cfg.lon_deg !== undefined && cfg.lon_frac !== undefined) {
        setVal('cfg-lon', (cfg.lon_deg + cfg.lon_frac / 1000.0).toFixed(3));
      }
      applyRhCtrl();
    });
}

// ── Config write ─────────────────────────────────────────────────────────────
function postCfg(ns, key, inputId, type) {
  const el = document.getElementById(inputId);
  if (!el) return;
  const value = type === 'int' ? parseInt(el.value, 10) : parseFloat(el.value);
  if (isNaN(value)) return;
  post('/api/config', { ns, key, value })
    .then(r => feedback('fb-' + inputId.replace('cfg-',''), r && r.ok));
}

function postCfgSelect(ns, key, inputId) {
  const el = document.getElementById(inputId);
  if (!el) return;
  const value = parseInt(el.value, 10);
  post('/api/config', { ns, key, value })
    .then(r => feedback('fb-' + inputId.replace('cfg-',''), r && r.ok));
  if (inputId === 'cfg-rh-ctrl-en') applyRhCtrl();
}

function applyRhCtrl() {
  const el = document.getElementById('cfg-rh-ctrl-en');
  const enabled = el && el.value === '1';
  document.querySelectorAll('.rh-dep').forEach(function(row) {
    row.classList.toggle('rh-disabled', !enabled);
  });
}

function postCfgStr(ns, key, inputId) {
  const el = document.getElementById(inputId);
  if (!el) return;
  post('/api/config', { ns, key, str_value: el.value })
    .then(r => feedback('fb-' + inputId.replace('cfg-',''), r && r.ok));
}

function postLocation() {
  const lat = parseFloat(document.getElementById('cfg-lat').value);
  const lon = parseFloat(document.getElementById('cfg-lon').value);
  if (isNaN(lat) || isNaN(lon)) return;
  const lat_deg = Math.trunc(lat), lat_frac = Math.round(Math.abs(lat - lat_deg) * 1000);
  const lon_deg = Math.trunc(lon), lon_frac = Math.round(Math.abs(lon - lon_deg) * 1000);
  Promise.all([
    post('/api/config', { ns: 'system', key: 'lat_deg',  value: lat_deg  }),
    post('/api/config', { ns: 'system', key: 'lat_frac', value: lat_frac }),
    post('/api/config', { ns: 'system', key: 'lon_deg',  value: lon_deg  }),
    post('/api/config', { ns: 'system', key: 'lon_frac', value: lon_frac }),
  ]).then(rs => feedback('fb-location', rs.every(r => r && r.ok)));
}

function postWifi() {
  const ssid = document.getElementById('cfg-wifi-ssid').value;
  const psk  = document.getElementById('cfg-wifi-psk').value;
  const body = { ssid };
  if (psk) body.psk = psk;   // omit PSK when blank — keeps stored password unchanged
  post('/api/wifi', body)
    .then(r => feedback('fb-wifi', r && r.ok));
}

function postApPsk() {
  const psk = document.getElementById('cfg-ap-psk').value;
  post('/api/wifi', { ap_psk: psk })
    .then(r => feedback('fb-ap-psk', r && r.ok));
}

function postPinChange(role) {
  const inputId = 'cfg-pin-' + role;
  const fbId    = 'fb-pin-' + role;
  const el = document.getElementById(inputId);
  if (!el || !el.value) return;
  post('/api/pin', { role, pin: el.value })
    .then(r => {
      feedback(fbId, r && r.ok);
      if (r && r.ok) el.value = '';
    });
}

// ── Sensor history ───────────────────────────────────────────────────────────
function loadHistory() {
  fetch('/api/history?n=60')
    .then(r => r.ok ? r.json() : null)
    .then(data => {
      if (!data || !data.rows) return;
      const tbody = document.getElementById('log-body');
      if (!tbody) return;
      tbody.innerHTML = '';
      data.rows.forEach(function(row) {
        const tr = document.createElement('tr');
        const t = row.ts ? new Date(row.ts * 1000).toLocaleTimeString() : '—';
        tr.innerHTML =
          '<td>' + esc(t) + '</td>' +
          '<td>' + (row.temp_c !== undefined ? row.temp_c.toFixed(1) : '—') + '</td>' +
          '<td>' + (row.rh_pct !== undefined ? row.rh_pct : '—') + '</td>' +
          '<td>' + (row.wind_ms !== undefined ? row.wind_ms.toFixed(1) : '—') + '</td>' +
          '<td>' + (row.wind_dir !== undefined ? row.wind_dir : '—') + '</td>';
        tbody.appendChild(tr);
      });
    });
}

// ── Tabs ──────────────────────────────────────────────────────────────────────
function showTab(id) {
  document.querySelectorAll('.tab-pane').forEach(p => p.classList.remove('active'));
  document.querySelectorAll('.tab-btn').forEach(b => b.classList.remove('active'));
  const pane = document.getElementById(id);
  if (pane) pane.classList.add('active');
  // Find matching button by onclick attr
  document.querySelectorAll('.tab-btn').forEach(b => {
    if (b.getAttribute('onclick') === "showTab('" + id + "')") b.classList.add('active');
  });
}

// ── Helpers ──────────────────────────────────────────────────────────────────
function post(url, body) {
  return fetch(url, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(body),
  }).then(r => r.ok ? r.json() : null).catch(() => null);
}

function setText(id, val) {
  const el = document.getElementById(id);
  if (el && val !== undefined && val !== null) el.textContent = val;
}

function setVal(id, val) {
  const el = document.getElementById(id);
  if (!el || val === undefined || val === null) return;
  el.value = val;
  // Keep paired slider in sync
  const sl = document.getElementById(id + '-sl');
  if (sl) sl.value = val;
}

function setBadge(id, text, cls) {
  const el = document.getElementById(id);
  if (!el) return;
  el.textContent = text;
  el.className   = 'badge ' + cls;
}

function feedback(fbId, ok) {
  const el = document.getElementById(fbId);
  if (!el) return;
  el.textContent = ok ? '✓ Saved' : '✗ Error';
  el.className   = ok ? 'save-ok' : 'save-err';
  setTimeout(() => { el.textContent = ''; el.className = 'save-ok'; }, 3000);
}

function esc(str) {
  return String(str)
    .replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
}

// ── Slider ↔ number sync ─────────────────────────────────────────────────────
function linkSlider(numId) {
  const num = document.getElementById(numId);
  const sl  = document.getElementById(numId + '-sl');
  if (!num || !sl) return;
  sl.addEventListener('input', function() { num.value = sl.value; });
  num.addEventListener('input', function() { sl.value = num.value; });
}

(function linkAllSliders() {
  [
    'cfg-t-max-day', 'cfg-t-min-day', 'cfg-rh-max-day', 'cfg-rh-min-day',
    'cfg-t-max-ngt', 'cfg-t-min-ngt', 'cfg-rh-max-ngt', 'cfg-rh-min-ngt',
    'cfg-hyst-t', 'cfg-hyst-rh', 'cfg-avg-win-t', 'cfg-avg-win-rh',
    'cfg-v-max', 'cfg-dir-excl-low', 'cfg-dir-excl-high',
    'cfg-travel-0', 'cfg-travel-1', 'cfg-travel-2',
    'cfg-dwell-open-0', 'cfg-dwell-open-1', 'cfg-dwell-open-2',
    'cfg-dwell-close-0', 'cfg-dwell-close-1', 'cfg-dwell-close-2',
    'cfg-session-timeout', 'cfg-ap-timeout', 'cfg-poll-interval',
  ].forEach(linkSlider);
})();

// ── Check session on load ────────────────────────────────────────────────────
fetch('/api/whoami')
  .then(r => r.ok ? r.json() : null)
  .then(r => {
    if (r && r.role) {
      setRole(r.role);
    }
    // Connect WS regardless — status is read-only and always visible
    wsConnect();
  })
  .catch(() => wsConnect());
