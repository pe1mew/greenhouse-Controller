'use strict';

// ── Auth state ───────────────────────────────────────────────────────────────
let g_role = null;  // 'farmer' | 'admin' | null

// ── Session idle timer ───────────────────────────────────────────────────────
// g_session_timeout_ms is loaded from cfg.session_timeout_min; default 5 min.
// g_last_activity is updated on every real user gesture.
// When the user has been idle for >= g_session_timeout_ms the client calls
// doLogout() which invalidates the server session and shows the login form.
let g_session_timeout_ms = 5 * 60 * 1000;
let g_last_activity      = Date.now();
['click', 'keydown', 'touchstart'].forEach(function (ev) {
  document.addEventListener(ev, function () { g_last_activity = Date.now(); },
                            { passive: true, capture: true });
});

function setRole(role) {
  g_role = role;
  document.body.classList.toggle('is-farmer', role === 'farmer' || role === 'admin');
  document.body.classList.toggle('is-admin',  role === 'admin');

  const rb      = document.getElementById('role-badge');
  const btnIn   = document.getElementById('btn-login');
  const btnOut  = document.getElementById('btn-logout');
  const settings = document.getElementById('section-settings');

  if (rb) {
    rb.textContent = role ? role.charAt(0).toUpperCase() + role.slice(1) : '';
    rb.style.display = role ? 'inline-block' : 'none';
    rb.style.background = role === 'admin' ? '#0f3460' : '#1a5276';
  }
  if (btnIn)    btnIn.style.display    = role ? 'none'         : 'inline-block';
  if (btnOut)   btnOut.style.display   = role ? 'inline-block' : 'none';
  if (settings) settings.style.display = role ? 'block'        : 'none';

  if (role) {
    loadConfig();
    loadSdStatus();
    if (role === 'admin') { loadOtaStatus(); loadLogFiles(); }
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
  // Canonical nested shape — single contract for local UI + public dashboard.
  // Field names match the dashboard's app.js (see design/technical-spec-statusWebsite.md
  // and the pe1mew.nl/hbwv reference frontend).
  const c = s.climate;
  if (c) {
    if (c.temp_c          !== undefined) setText('st-temp',     c.temp_c.toFixed(1));
    if (c.temp_avg_c      !== undefined) setText('st-temp-avg', c.temp_avg_c.toFixed(1));
    if (c.rh_pct          !== undefined) setText('st-rh',       c.rh_pct.toFixed(0));
    if (c.rh_avg_pct      !== undefined) setText('st-rh-avg',   c.rh_avg_pct.toFixed(0));
    if (c.temp_max_active !== undefined) setText('st-t-max',    c.temp_max_active);
    if (c.rh_max_active   !== undefined) setText('st-rh-max',   c.rh_max_active);
    if (c.rh_min_active   !== undefined) setText('st-rh-min',   c.rh_min_active);
    // Dim the Humidity setpoint rows when RH control is disabled. The
    // values stay visible so the operator can still see what is configured
    // for the moment they re-enable the control. Public dashboard never
    // sees these fields when disabled (T14 omits them — see build_canonical_status_json
    // include_disabled_setpoints=false).
    if (c.rh_ctrl_enabled !== undefined) {
      const dim = !c.rh_ctrl_enabled;
      ['st-rh-max', 'st-rh-min'].forEach(function (id) {
        const el = document.getElementById(id);
        if (el && el.parentElement) el.parentElement.classList.toggle('dimmed', dim);
      });
    }
  }

  const w = s.wind;
  if (w) {
    if (w.speed_ms                !== undefined) setText('st-wind',     w.speed_ms.toFixed(1));
    if (w.speed_avg_ms            !== undefined) setText('st-wind-avg', w.speed_avg_ms.toFixed(1));
    if (w.direction_deg           !== undefined) setText('st-wind-dir', w.direction_deg.toFixed(0));
    if (w.direction_variation_deg !== undefined) setText('st-wind-var', w.direction_variation_deg.toFixed(0));
  }

  // Windows — object keyed M1/M2/M3
  if (s.windows) {
    const ids = ['M1', 'M2', 'M3'];
    for (let i = 0; i < 3; i++) {
      const el = document.getElementById('st-win' + i);
      if (!el) continue;
      const st = s.windows[ids[i]] || 'UNKNOWN';
      el.textContent = WIN_LABELS[st] || st;
      el.className = WIN_CLASS[st] || '';
    }
  }

  // Mode + Alarms — the Alarms card aggregates every active concern. Mode
  // flags from the EG1 bitset come first; a version-mismatch (firmware
  // running against stale web-assets after an incomplete OTA) appends a
  // MISMATCH badge to the same list. Both surfaces share the same DOM
  // element so the Alarms card is the single place to look for trouble.
  const sys = s.system;
  let alarmBadges = [];
  if (s.mode && typeof s.mode === 'object') {
    const modeNames = {
      AUTOMATIC: 'Automatic', STANDBY: 'Standby',
      WIND_OVERRIDE: 'Wind override', MOTOR_ALARM: 'Motor alarm',
      WINDOW_CAL: 'Window Cal.'
    };
    if (s.mode.current) setText('st-mode', modeNames[s.mode.current] || s.mode.current);

    const flagBadges = {
      wind_override:      '<span class="badge alarm">WIND</span>',
      motor_alarm:        '<span class="badge alarm">MOTOR ALARM</span>',
      sensor_fault_temp:  '<span class="badge warn">T/RH fault</span>',
      sensor_fault_wind:  '<span class="badge warn">Wind fault</span>',
      ota_in_progress:    '<span class="badge warn">OTA active</span>',
      calibrating:        '<span class="badge warn">Calibrating</span>',
      // gh#18 Phase 1 — T14 circuit breaker open. Phase 1 never emits this
      // flag (stub returns false); Phase 2 wires it to real breaker state.
      net_backoff_active: '<span class="badge warn">Net backoff</span>'
    };
    alarmBadges = (Array.isArray(s.mode.flags) ? s.mode.flags : [])
      .map(f => flagBadges[f]).filter(x => x);
  }
  // Append MISMATCH when firmware and asset versions disagree. '?' (no
  // manifest on the active LFS — happens right after a clean serial flash)
  // is treated as "unknown" and never triggers the badge.
  if (sys && sys.fw_ver && sys.asset_version &&
      sys.asset_version !== '?' && sys.fw_ver !== sys.asset_version) {
    alarmBadges.push('<span class="badge alarm">MISMATCH</span>');
  }
  {
    const el = document.getElementById('st-alarms');
    if (el) el.innerHTML = alarmBadges.length
        ? alarmBadges.join(' ')
        : '<span class="badge ok">OK</span>';
  }

  // Sun
  const sun = s.sun;
  if (sun) {
    if (sun.is_daytime  !== undefined) setText('st-daytime', sun.is_daytime ? 'Daytime' : 'Night');
    if (sun.sunrise_min !== undefined) setText('st-sunrise', utcMinsToStr(sun.sunrise_min));
    if (sun.sunset_min  !== undefined) setText('st-sunset',  utcMinsToStr(sun.sunset_min));
  }

  // System: time, NTP, IP/RSSI, uptime, firmware version
  if (sys) {
    if (sys.time_iso) setText('st-time', sys.time_iso.replace('T', ' '));
    const ntpEl = document.getElementById('st-ntp');
    if (ntpEl) {
      ntpEl.textContent = sys.ntp_synced ? 'NTP synced' : 'NTP pending';
      ntpEl.className   = 'badge ' + (sys.ntp_synced ? 'ntp-on' : 'ntp-off');
    }
    if (sys.wifi_rssi_dbm !== undefined) setText('st-wifi-rssi', sys.wifi_rssi_dbm);
    if (sys.wifi_ip)                     setText('st-wifi-ip',   sys.wifi_ip);
    if (sys.uptime_s !== undefined)      setText('st-uptime',    fmtUptime(sys.uptime_s));
    // Firmware version goes into the page footer. Set on every push
    // (idempotent setText) so it remains correct after a re-render and
    // there is no first-message gating window where the field stays at "—".
    if (sys.fw_ver) setText('fw-ver', 'v' + sys.fw_ver);
    // sys.asset_version is consumed by the Alarms-card mismatch check
    // above; no separate visible field — a mismatch shows up as the
    // MISMATCH badge alongside the mode-derived alarms.
  }

  wsInitialized = true;
}

// Format seconds as "1d 4h 23m" / "4h 23m" / "23m 5s" / "5s". Drops leading
// units that are 0 so short uptimes stay readable. Used in the System tile.
function fmtUptime(sec) {
  if (typeof sec !== 'number' || sec < 0) return '—';
  const d = Math.floor(sec / 86400);
  const h = Math.floor((sec % 86400) / 3600);
  const m = Math.floor((sec % 3600) / 60);
  const s = Math.floor(sec % 60);
  if (d > 0) return d + 'd ' + h + 'h ' + m + 'm';
  if (h > 0) return h + 'h ' + m + 'm';
  if (m > 0) return m + 'm ' + s + 's';
  return s + 's';
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
        hideLoginModal();
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
  const modal = document.getElementById('login-modal');
  if (e.key === 'Enter' && modal && modal.style.display !== 'none') {
    doLogin();
  }
});

function showLoginModal() {
  const modal = document.getElementById('login-modal');
  if (modal) modal.style.display = 'flex';
  const pinEl = document.getElementById('login-pin');
  if (pinEl) pinEl.focus();
}

function hideLoginModal() {
  const modal = document.getElementById('login-modal');
  if (modal) modal.style.display = 'none';
}

function modalBackdropClick(event) {
  // Close modal only when the semi-transparent backdrop is clicked,
  // not when clicking inside the login-box itself.
  if (event.target === document.getElementById('login-modal')) {
    hideLoginModal();
  }
}

function showLogin() {
  // Session expired or logged out — drop back to unauthenticated state.
  setRole(null);
  wsInitialized = false;
}

function doLogout() {
  post('/api/logout', {}).then(() => setRole(null));
}

// Periodic session check — two responsibilities:
//   1. Client-side idle logout: if the user has not interacted for
//      g_session_timeout_ms, call doLogout() to invalidate the session
//      on the server and return to the login form.
//   2. Server-side validity probe: fetch /api/whoami (which does NOT slide
//      the server-side expiry) to detect forced logout or device reboot
//      while the user is still active.
setInterval(function () {
  if (g_role === null) return;
  if (Date.now() - g_last_activity >= g_session_timeout_ms) {
    doLogout();   // idle timeout reached — log out cleanly
    return;
  }
  fetch('/api/whoami')
    .then(function (r) { if (!r.ok) showLogin(); })
    .catch(function () {});
}, 60000);

// Periodic config refresh — keeps Settings inputs in sync with NVS so that
// changes made via the REST API (test scripts, other clients) are reflected
// in the GUI without a full page reload.  Fields the user is currently editing
// are skipped (see setVal).
// Only runs while the user is active so that background polls do not silently
// extend the server-side session when nobody is at the keyboard.
setInterval(function () {
  if (g_role === null) return;
  if (Date.now() - g_last_activity < g_session_timeout_ms) loadConfig();
}, 60000);

// Periodic history refresh — public endpoint, no auth required.
setInterval(function () {
  loadHistory();
}, 120000);

// ── Config load ──────────────────────────────────────────────────────────────
function loadConfig() {
  fetch('/api/config')
    .then(function (r) {
      if (r.status === 401) { showLogin(); return null; }
      return r.ok ? r.json() : null;
    })
    .then(cfg => {
      if (!cfg) return;
      setVal('cfg-t-max-day',      cfg.t_max_day);
      // HEATING CONTROL NOT IMPLEMENTED — preserved for future use
      // setVal('cfg-t-min-day',   cfg.t_min_day);
      setVal('cfg-t-max-ngt',      cfg.t_max_ngt);
      // HEATING CONTROL NOT IMPLEMENTED — preserved for future use
      // setVal('cfg-t-min-ngt',   cfg.t_min_ngt);
      setVal('cfg-rh-max-day',     cfg.rh_max_day);
      setVal('cfg-rh-min-day',     cfg.rh_min_day);
      setVal('cfg-rh-max-ngt',     cfg.rh_max_ngt);
      setVal('cfg-rh-min-ngt',     cfg.rh_min_ngt);
      setVal('cfg-hyst-t',         cfg.hyst_t);
      setVal('cfg-hyst-rh',        cfg.hyst_rh);
      setVal('cfg-avg-win-t',      cfg.avg_win_t);
      setVal('cfg-avg-win-rh',     cfg.avg_win_rh);
      setVal('cfg-rh-ctrl-en',     String(cfg.rh_ctrl_en));
      setVal('cfg-cr-priority',    String(cfg.cr_priority));
      setVal('cfg-v-max',          cfg.v_max);
      setVal('cfg-dir-excl-low',   cfg.dir_excl_low);
      setVal('cfg-dir-excl-high',  cfg.dir_excl_high);
      setVal('cfg-wind-prot-en',   String(cfg.wind_prot_en));
      setVal('cfg-travel-m1',       cfg.travel_s && cfg.travel_s[0]);
      setVal('cfg-travel-m2',       cfg.travel_s && cfg.travel_s[1]);
      setVal('cfg-travel-m3',       cfg.travel_s && cfg.travel_s[2]);
      setVal('cfg-dwell-open-m1',   cfg.dwell_open_min && cfg.dwell_open_min[0]);
      setVal('cfg-dwell-open-m2',   cfg.dwell_open_min && cfg.dwell_open_min[1]);
      setVal('cfg-dwell-open-m3',   cfg.dwell_open_min && cfg.dwell_open_min[2]);
      setVal('cfg-dwell-close-m1',  cfg.dwell_close_min && cfg.dwell_close_min[0]);
      setVal('cfg-dwell-close-m2',  cfg.dwell_close_min && cfg.dwell_close_min[1]);
      setVal('cfg-dwell-close-m3',  cfg.dwell_close_min && cfg.dwell_close_min[2]);
      setVal('cfg-session-timeout', cfg.session_timeout_min);
      g_session_timeout_ms = (cfg.session_timeout_min > 0 ? cfg.session_timeout_min : 5) * 60 * 1000;
      setVal('cfg-ap-timeout',     cfg.ap_timeout_min);
      setVal('cfg-poll-interval',  cfg.poll_interval_s);
      setVal('cfg-wifi-ssid',      cfg.wifi_ssid);
      setText('cfg-ap-ssid',       cfg.ap_ssid);
      setVal('cfg-tz',             cfg.tz_str);
      if (cfg.fw_ver) setText('fw-ver', 'v' + cfg.fw_ver);
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
    .then(function (r) {
      return r.ok ? r.json() : Promise.reject('HTTP ' + r.status);
    })
    .then(function(data) {
      if (!data || !data.rows) return;
      const tbody = document.getElementById('log-body');
      if (!tbody) return;
      tbody.innerHTML = '';
      data.rows.slice().reverse().forEach(function(row) {
        const tr = document.createElement('tr');
        const t = row.ts ? new Date(row.ts * 1000).toLocaleTimeString() : '—';
        // Helpers for compact "value or dash" cells, with .toFixed(1) on
        // float-style fields and integer rendering on RH / direction.
        const f1 = v => (v !== undefined ? v.toFixed(1) : '—');
        const i0 = v => (v !== undefined ? v             : '—');
        tr.innerHTML =
          '<td>' + esc(t) + '</td>' +
          '<td>' + f1(row.temp_c)                  + '</td>' +
          '<td>' + f1(row.temp_avg_c)              + '</td>' +
          '<td>' + i0(row.rh_pct)                  + '</td>' +
          '<td>' + i0(row.rh_avg_pct)              + '</td>' +
          '<td>' + f1(row.speed_ms)                + '</td>' +
          '<td>' + f1(row.speed_avg_ms)            + '</td>' +
          '<td>' + i0(row.direction_deg)           + '</td>' +
          '<td>' + i0(row.direction_variation_deg) + '</td>';
        tbody.appendChild(tr);
      });
    })
    .catch(function(err) {
      console.warn('loadHistory failed:', err);
    });
}

// ── SD card ──────────────────────────────────────────────────────────────────
function loadSdStatus() {
  fetch('/api/sd/status')
    .then(function (r) {
      return r.ok ? r.json() : null;
    })
    .then(function (sd) {
      if (!sd) return;
      const statusEl = document.getElementById('st-sd-status');
      if (statusEl) {
        statusEl.textContent = sd.mounted ? 'Mounted' : 'Not mounted';
        statusEl.style.color = sd.mounted ? 'var(--green)' : 'var(--muted)';
      }
      setText('st-sd-size', sd.mounted ? sd.size_mb + ' MB' : '—');
      setText('st-sd-free', sd.mounted ? sd.free_mb + ' MB' : '—');

      const mountBtn   = document.getElementById('btn-sd-mount');
      const unmountBtn = document.getElementById('btn-sd-unmount');
      if (mountBtn)   mountBtn.disabled   = sd.mounted;
      if (unmountBtn) unmountBtn.disabled = !sd.mounted;
    });
}

function postSdMount() {
  post('/api/sd/mount', {})
    .then(function (r) {
      feedback('fb-sd', r && r.ok);
      if (r && r.ok) loadSdStatus();
    });
}

function postSdUnmount() {
  post('/api/sd/unmount', {})
    .then(function (r) {
      feedback('fb-sd', r && r.ok);
      if (r && r.ok) loadSdStatus();
    });
}

// Periodic SD status refresh — public endpoint, no auth required.
setInterval(function () {
  loadSdStatus();
}, 30000);

// ── OTA update ───────────────────────────────────────────────────────────────
// State names returned by GET /api/ota/status that indicate active operation.
var OTA_ACTIVE_STATES = ['fw_writing', 'fw_verifying', 'fw_done',
                         'assets_buffering', 'assets_writing', 'rebooting'];
var g_ota_poll_timer  = null;

function loadOtaStatus() {
  fetch('/api/ota/status')
    .then(function (r) {
      if (r.status === 401) { showLogin(); return null; }
      return r.ok ? r.json() : null;
    })
    .then(function (data) {
      if (!data) return;
      var statusEl  = document.getElementById('ota-status-text');
      var wrapEl    = document.getElementById('ota-progress-wrap');
      var fillEl    = document.getElementById('ota-progress-fill');
      var label = data.state.replace(/_/g, ' ');
      if (data.state === 'idle') {
        var bankStr = data.bank ? ('Bank ' + data.bank) : '';
        var accStr  = (data.accepted === true)  ? 'accepted' :
                      (data.accepted === false) ? 'not yet accepted' : '';
        label = 'Idle';
        if (bankStr || accStr) label += ' — ' + [bankStr, accStr].filter(Boolean).join(', ');
      }
      if (data.state === 'error' && data.error) label += ': ' + data.error;
      if (data.state === 'rebooting') label = 'Rebooting…';
      if (data.state === 'fw_done')   label = 'Firmware ready — please upload the web assets ZIP';
      if (statusEl) statusEl.textContent = label.charAt(0).toUpperCase() + label.slice(1);
      var active  = OTA_ACTIVE_STATES.indexOf(data.state) !== -1;
      /* Also keep polling after reboot until the firmware is accepted (~35 s). */
      var pending = (data.state === 'idle' && data.accepted === false);
      if (wrapEl) wrapEl.style.display = active ? 'block' : 'none';
      if (fillEl) fillEl.style.width = data.progress + '%';
      if (active || pending) {
        if (!g_ota_poll_timer) {
          g_ota_poll_timer = setTimeout(function () {
            g_ota_poll_timer = null;
            loadOtaStatus();
          }, active ? 2000 : 5000);
        }
      } else {
        if (g_ota_poll_timer) { clearTimeout(g_ota_poll_timer); g_ota_poll_timer = null; }
      }
    })
    .catch(function () {
      /* Connection failed — device has rebooted.  Stop polling and prompt reload. */
      if (g_ota_poll_timer) { clearTimeout(g_ota_poll_timer); g_ota_poll_timer = null; }
      var statusEl  = document.getElementById('ota-status-text');
      var wrapEl    = document.getElementById('ota-progress-wrap');
      var fillEl    = document.getElementById('ota-progress-fill');
      if (statusEl) statusEl.textContent = 'Rebooting — reload the page once the device comes back online';
      if (fillEl)   fillEl.style.width   = '100%';
      if (wrapEl)   wrapEl.style.display = 'block';
    });
}

/**
 * Upload a firmware .bin as a raw binary POST body.
 * The firmware is streamed directly to the inactive OTA partition.
 */
function uploadOtaFirmware() {
  var fileEl = document.getElementById('ota-fw-file');
  if (!fileEl || !fileEl.files || !fileEl.files[0]) {
    feedback('fb-ota-fw', false); return;
  }
  var file = fileEl.files[0];
  var statusEl = document.getElementById('ota-status-text');
  if (statusEl) statusEl.textContent = 'Uploading firmware (' + Math.round(file.size / 1024) + ' kB)…';

  fetch('/api/ota/firmware', {
    method: 'POST',
    headers: { 'Content-Type': 'application/octet-stream' },
    body: file,
  })
    .then(function (r) {
      if (!r.ok) return null;
      return r.json();
    })
    .then(function (data) {
      feedback('fb-ota-fw', data && data.ok);
      if (data && data.ok) {
        if (statusEl) statusEl.textContent = 'Firmware ready — please upload the web assets ZIP';
        loadOtaStatus();
      }
    })
    .catch(function () { feedback('fb-ota-fw', false); });
}

/**
 * Upload a STORE-only web-assets .zip as a raw binary POST body.
 * The ZIP is buffered in PSRAM on the device then T13 extracts it.
 * Poll /api/ota/status for extraction progress.
 */
function uploadOtaAssets() {
  var fileEl = document.getElementById('ota-assets-file');
  if (!fileEl || !fileEl.files || !fileEl.files[0]) {
    feedback('fb-ota-assets', false); return;
  }
  var file = fileEl.files[0];
  var statusEl = document.getElementById('ota-status-text');
  if (statusEl) statusEl.textContent = 'Uploading assets ZIP (' + Math.round(file.size / 1024) + ' kB)…';

  fetch('/api/ota/assets', {
    method: 'POST',
    headers: { 'Content-Type': 'application/zip' },
    body: file,
  })
    .then(function (r) {
      if (r.status === 202) return r.json();
      if (!r.ok) return null;
      return r.json();
    })
    .then(function (data) {
      feedback('fb-ota-assets', data && data.ok);
      if (data && data.ok) {
        // Start polling for extraction progress
        if (g_ota_poll_timer) clearTimeout(g_ota_poll_timer);
        g_ota_poll_timer = setTimeout(function () {
          g_ota_poll_timer = null;
          loadOtaStatus();
        }, 1000);
      }
    })
    .catch(function () { feedback('fb-ota-assets', false); });
}

// ── Log tab ──────────────────────────────────────────────────────────────────
function loadLogFiles() {
  var sel = document.getElementById('log-src-select');
  if (!sel) return;
  fetch('/api/log/files')
    .then(function (r) {
      if (r.status === 401) { showLogin(); return null; }
      return r.ok ? r.json() : null;
    })
    .then(function (data) {
      if (!data) return;
      sel.innerHTML = '';
      // NVS buffer option (always present)
      var opt = document.createElement('option');
      opt.value = 'nvs';
      opt.textContent = 'NVS buffer (' + data.nvs_count + ' entries)';
      sel.appendChild(opt);
      // SD file options
      if (data.sd_files && data.sd_files.length > 0) {
        data.sd_files.forEach(function (fname) {
          var o = document.createElement('option');
          o.value = 'sd:' + fname;
          o.textContent = fname;
          sel.appendChild(o);
        });
      }
    });
}

function downloadLog() {
  var sel = document.getElementById('log-src-select');
  if (!sel || !sel.value) { feedback('fb-log-dl', false); return; }
  var val = sel.value;
  var url;
  if (val === 'nvs') {
    url = '/api/log/download?src=nvs';
  } else if (val.indexOf('sd:') === 0) {
    url = '/api/log/download?src=sd&file=' + encodeURIComponent(val.slice(3));
  } else {
    feedback('fb-log-dl', false); return;
  }
  // Trigger browser file download without navigating away
  var a = document.createElement('a');
  a.href = url;
  a.download = '';
  document.body.appendChild(a);
  a.click();
  document.body.removeChild(a);
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
  // Refresh log file list each time the Log tab is opened
  if (id === 'tab-log') loadLogFiles();
  // Refresh OTA status each time the System tab is opened
  if (id === 'tab-system' && g_role === 'admin') loadOtaStatus();
  // Refresh status-website settings + last-attempt indicators on the Web tab
  if (id === 'tab-web'    && g_role === 'admin') loadWebCfg();
}

// ── Web tab — status website reporting ────────────────────────────────────────
const WEB_TILES = ['climate','wind','windows','mode','sun','system'];

// Full reload: pulls every field including form inputs. Called on Apply
// success and when the Web tab is first opened. The 5 s auto-refresh uses
// refreshWebStatus() instead to avoid clobbering the user's edits.
function loadWebCfg() { fetchWebCfg(true); }
function refreshWebStatus() { fetchWebCfg(false); }

function fetchWebCfg(includeInputs) {
  fetch('/api/web', { credentials: 'same-origin' })
    .then(r => r.ok ? r.json() : null)
    .then(c => {
      if (!c) return;
      const set = (id, v) => { const el = document.getElementById(id); if (el) el.value = v; };
      const chk = (id, v) => { const el = document.getElementById(id); if (el) el.checked = !!v; };
      const txt = (id, v) => { const el = document.getElementById(id); if (el) el.textContent = v || '—'; };

      if (includeInputs) {
        set('cfg-web-url',      c.url || '');
        set('cfg-web-secret',   '');                          // never echoed
        set('cfg-web-interval', c.interval_s != null ? c.interval_s : 120);
        chk('cfg-web-enable',   c.enable);
        WEB_TILES.forEach((k, i) => chk('cfg-web-exp-' + k, (c.expose & (1 << i)) !== 0));
        set('cfg-web-log-h',    c.log_h != null ? c.log_h : 3);
        set('cfg-web-log-m',    c.log_m != null ? c.log_m : 15);
        chk('cfg-web-log-rot',  c.log_rot);
      }
      // Read-only status indicators are always refreshed.
      txt('cfg-web-last-post', c.last_post);
      txt('cfg-web-last-log',  c.last_log_up);
      txt('cfg-web-last-name', c.log_last_up);
    });
}

// Auto-refresh the Web tab's read-only status indicators every 5 s while
// it is the active tab and the user is admin. Calls refreshWebStatus()
// (NOT loadWebCfg) so the user's in-progress edits to URL / secret /
// interval / checkboxes are not clobbered mid-typing. The check is cheap
// when off-tab — one classList read per tick, no fetch.
setInterval(() => {
  if (g_role !== 'admin') return;
  const pane = document.getElementById('tab-web');
  if (pane && pane.classList.contains('active')) refreshWebStatus();
}, 5000);

// Client-side syntax check for the status-site URL. Empty is allowed (it
// disables the feature server-side). Otherwise the URL must use http(s)://,
// must not carry a query string or fragment (T14 appends ?action=log), and
// must end with "api.php" — Apache routing varies and the firmware does not
// follow redirects, so requiring the exact endpoint avoids silent FAILs.
function validateStatusUrl(url) {
  if (url === '')                                return '';
  if (!/^https?:\/\//.test(url))                 return 'URL must start with http:// or https://';
  if (url.indexOf('?') !== -1 || url.indexOf('#') !== -1)
                                                  return 'URL must not contain ? or #';
  if (!url.endsWith('api.php'))                  return 'URL must end with "api.php"';
  return '';
}

function postWebCfg() {
  const get = id => document.getElementById(id);
  let mask = 0;
  WEB_TILES.forEach((k, i) => { if (get('cfg-web-exp-' + k).checked) mask |= (1 << i); });

  const url = get('cfg-web-url').value.trim();
  const fb  = document.getElementById('fb-web');
  const urlErr = validateStatusUrl(url);
  if (urlErr) {
    if (fb) { fb.textContent = urlErr; fb.className = 'save-ok err'; }
    return;
  }

  const body = {
    url:        url,
    secret:     get('cfg-web-secret').value,                // empty = unchanged
    interval_s: parseInt(get('cfg-web-interval').value, 10),
    enable:     get('cfg-web-enable').checked ? 1 : 0,
    expose:     mask,
    log_h:      parseInt(get('cfg-web-log-h').value, 10),
    log_m:      parseInt(get('cfg-web-log-m').value, 10),
    log_rot:    get('cfg-web-log-rot').checked ? 1 : 0
  };

  if (fb) fb.textContent = '…';
  post('/api/web', body)
    .then(r => {
      if (!fb) return;
      if (r && r.ok)        { fb.textContent = 'Saved'; fb.className = 'save-ok ok'; }
      else if (r && r.err)  { fb.textContent = r.err;   fb.className = 'save-ok err'; }
      else                  { fb.textContent = 'Failed'; fb.className = 'save-ok err'; }
      setTimeout(() => { fb.textContent = ''; fb.className = 'save-ok'; }, 3000);
      // Clear the secret field so the next Apply doesn't re-send it.
      get('cfg-web-secret').value = '';
      // Pull the canonical values back from the device so the form reflects
      // exactly what was persisted (not just what the user typed).
      if (r && r.ok) loadWebCfg();
    });
}

// ── Helpers ──────────────────────────────────────────────────────────────────
function post(url, body) {
  return fetch(url, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(body),
  }).then(function (r) {
    if (r.status === 401) { showLogin(); return null; }
    return r.ok ? r.json() : null;
  }).catch(function () { return null; });
}

function setText(id, val) {
  const el = document.getElementById(id);
  if (el && val !== undefined && val !== null) el.textContent = val;
}

function setVal(id, val) {
  const el = document.getElementById(id);
  if (!el || val === undefined || val === null) return;
  // Do not overwrite a field (or its paired slider) that the user is currently editing.
  if (el === document.activeElement) return;
  el.value = val;
  // Keep paired slider in sync
  const sl = document.getElementById(id + '-sl');
  if (sl && sl !== document.activeElement) sl.value = val;
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
    'cfg-t-max-day', /* 'cfg-t-min-day', HEATING CONTROL NOT IMPLEMENTED — preserved for future use */
    'cfg-rh-max-day', 'cfg-rh-min-day',
    'cfg-t-max-ngt', /* 'cfg-t-min-ngt', HEATING CONTROL NOT IMPLEMENTED — preserved for future use */
    'cfg-rh-max-ngt', 'cfg-rh-min-ngt',
    'cfg-hyst-t', 'cfg-hyst-rh', 'cfg-avg-win-t', 'cfg-avg-win-rh',
    'cfg-v-max', 'cfg-dir-excl-low', 'cfg-dir-excl-high',
    'cfg-travel-m1', 'cfg-travel-m2', 'cfg-travel-m3',
    'cfg-dwell-open-m1', 'cfg-dwell-open-m2', 'cfg-dwell-open-m3',
    'cfg-dwell-close-m1', 'cfg-dwell-close-m2', 'cfg-dwell-close-m3',
    'cfg-session-timeout', 'cfg-ap-timeout', 'cfg-poll-interval',
  ].forEach(linkSlider);
})();

// ── Config limits ────────────────────────────────────────────────────────────
// Fetches /api/config/limits once at page load (public endpoint, no auth).
// The response is a JSON object keyed by NVS parameter name, each value an
// [min, max] array.  The key name maps to the HTML input ID by replacing '_'
// with '-' and prepending 'cfg-', which matches every config input exactly
// (motor inputs were renamed from cfg-*-0/1/2 to cfg-*-m1/m2/m3 to align).
// The slider counterpart is found by appending '-sl' to the same base ID.
function loadLimits() {
  fetch('/api/config/limits')
    .then(function (r) { return r.ok ? r.json() : null; })
    .then(function (limits) {
      if (!limits) return;
      Object.keys(limits).forEach(function (key) {
        var range = limits[key];           // [min, max]
        var id  = 'cfg-' + key.replace(/_/g, '-');
        var num = document.getElementById(id);
        var sl  = document.getElementById(id + '-sl');
        if (num) { num.min = range[0]; num.max = range[1]; }
        if (sl)  { sl.min  = range[0]; sl.max  = range[1]; }
      });
    })
    .catch(function () { /* limits unavailable — inputs work without constraints */ });
}

// ── Initialise on load ───────────────────────────────────────────────────────
// Connect WebSocket and load sensor history immediately — both are public.
// Then check for an existing session so we can restore Settings if the user
// had already logged in before reloading the page.
loadLimits();
wsConnect();
loadHistory();
loadSdStatus();
fetch('/api/whoami')
  .then(r => r.ok ? r.json() : null)
  .then(r => { if (r && r.role) setRole(r.role); })
  .catch(function () {});
