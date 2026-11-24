/* BrewDial harness page. Talks to the firmware (compiled to WebAssembly) through the bd_* C API
 * in sim/harness/web_api.cpp. The page shows °F like the device UI; the firmware core is °C. */
'use strict';

const $ = (id) => document.getElementById(id);
const toF = (c) => c * 9 / 5 + 32;
const toC = (f) => (f - 32) * 5 / 9;
const dF = (f) => f * 5 / 9;            // a °F difference in °C
const fmtF = (c) => toF(c).toFixed(1) + '°F';

function hms(ms) {
  const s = Math.floor(ms / 1000);
  const d = Math.floor(s / 86400);
  const t = [Math.floor(s / 3600) % 24, Math.floor(s / 60) % 60, s % 60].map((n) => String(n).padStart(2, '0')).join(':');
  return d ? `${d}d ${t}` : t;
}
function dur(ms) {
  const s = ms / 1000;
  if (s < 90) return s.toFixed(0) + ' s';
  if (s < 5400) return (s / 60).toFixed(1) + ' min';
  return (s / 3600).toFixed(1) + ' h';
}

if (typeof createBrewDial !== 'function') {
  const l = $('loading');
  l.classList.add('err');
  l.textContent = 'brew_web.js not found.\nBuild it first (see sim/web/README.md), then open build-web/web/index.html.';
  throw new Error('brew_web.js missing');
}

createBrewDial().then((M) => {
  const api = {
    size: M._bd_size, start: M._bd_start, frame: M._bd_frame, rgba: M._bd_rgba, frames: M._bd_frame_count,
    turn: M._bd_turn, counts: M._bd_counts, button: M._bd_button, click: M._bd_click, touch: M._bd_touch,
    beer: M._bd_set_beer_c, physics: M._bd_set_physics, plant: M._bd_plant,
    fault: M._bd_set_fault, noise: M._bd_set_noise, offset: M._bd_set_offset,
    speed: M._bd_set_speed, reboot: M._bd_reboot, factory: M._bd_factory_reset, setup: M._bd_setup,
    split: M._bd_set_split, link: M._bd_link, key: M._bd_key, layout: M._bd_layout, graphStyle: M._bd_graph_style,
    status: () => JSON.parse(M.UTF8ToString(M._bd_status())),
    http: (method, path, body) => M.ccall('bd_http', 'number', ['string', 'string', 'string'], [method, path, body]),
    httpBody: () => M.UTF8ToString(M._bd_http_body()), httpType: () => M.UTF8ToString(M._bd_http_type()),
    wifiPhone: M._bd_wifi_phone, wifiRouter: M._bd_wifi_router, wifiSetup: M._bd_wifi_setup,
    wifiSkip: M._bd_wifi_skip, wifiForget: M._bd_wifi_forget,
    nextLog: () => M.UTF8ToString(M._bd_next_log()),
  };

  const N = api.size();
  const screen = $('screen');
  screen.width = N;
  screen.height = N;
  const ctx = screen.getContext('2d');
  const img = ctx.createImageData(N, N);

  api.start(performance.now());
  api.noise(dF(+$('noise').value));
  /* ?speed=600 starts fast (handy for screenshots and long soak runs) */
  const qs = new URLSearchParams(location.search);
  if (qs.has('speed')) {
    api.speed(+qs.get('speed'));
    document.querySelectorAll('#speed button').forEach((b) => b.classList.toggle('on', b.dataset.speed === qs.get('speed')));
  }
  $('loading').remove();

  /* ------------------------------------------------------------ main loop */
  let lastFrames = -1;
  let st = api.status();
  let lastStatus = 0, lastSample = 0;

  function loop(now) {
    api.frame(now);
    const f = api.frames();
    if (f !== lastFrames) {
      lastFrames = f;
      const p = api.rgba();
      img.data.set(M.HEAPU8.subarray(p, p + N * N * 4));
      ctx.putImageData(img, 0, 0);
    }
    if (now - lastStatus > 100) {
      lastStatus = now;
      st = api.status();
      render(st);
      drainLog();
    }
    if (now - lastSample > 200) {
      lastSample = now;
      sample(st);
      drawChart();
    }
    requestAnimationFrame(loop);
  }
  requestAnimationFrame(loop);

  /* ------------------------------------------------------------ knob ring + touch */
  /* The CrowPanel's knob is the ring around the glass: 30 detents per turn = 12° each. */
  const DETENT_DEG = 12;
  let kind = 'round';   // which device the stage shows: 'round' (knob ring) or 'square' (side keys)
  const ring = $('ring'), knurl = $('knurl'), device = $('device');
  let rot = 0;
  const spin = (deg) => { rot += deg; knurl.style.setProperty('--rot', rot + 'deg'); };
  const turn = (d) => { spin(d * DETENT_DEG); api.turn(d); };
  $('cw').onclick = () => turn(1);
  $('ccw').onclick = () => turn(-1);
  $('cw5').onclick = () => turn(5);
  $('ccw5').onclick = () => turn(-5);
  $('bounce').onclick = () => { spin(DETENT_DEG / 2); api.counts(1); };

  /* drag the ring itself: the ridges follow the pointer, every 12° is one detent */
  let drag = null;
  const angleAt = (e) => {
    const r = device.getBoundingClientRect();
    return Math.atan2(e.clientY - (r.top + r.height / 2), e.clientX - (r.left + r.width / 2)) * 180 / Math.PI;
  };
  ring.addEventListener('pointerdown', (e) => {
    ring.setPointerCapture(e.pointerId);
    ring.classList.add('dragging');
    drag = { last: angleAt(e), acc: 0 };
  });
  ring.addEventListener('pointermove', (e) => {
    if (!drag) return;
    const a = angleAt(e);
    let d = a - drag.last;
    if (d > 180) d -= 360;
    if (d < -180) d += 360;
    drag.last = a;
    drag.acc += d;
    spin(d);
    while (Math.abs(drag.acc) >= DETENT_DEG) {
      const s = Math.sign(drag.acc);
      api.turn(s);                 // clockwise on screen = clockwise on the knob
      drag.acc -= s * DETENT_DEG;
    }
  });
  const endDrag = () => {
    if (!drag) return;
    spin(-drag.acc);               // settle into the detent
    drag = null;
    ring.classList.remove('dragging');
  };
  ring.addEventListener('pointerup', endDrag);
  ring.addEventListener('pointercancel', endDrag);

  const press = $('press');
  const down = () => { press.classList.add('held'); device.classList.add('pressed'); api.button(1); };
  const up = () => {
    if (!press.classList.contains('held')) return;
    press.classList.remove('held');
    device.classList.remove('pressed');
    api.button(0);
  };
  press.addEventListener('pointerdown', (e) => { press.setPointerCapture(e.pointerId); down(); });
  press.addEventListener('pointerup', up);
  press.addEventListener('pointercancel', up);

  let wheelAcc = 0;
  const onWheel = (e) => {
    if (kind !== 'round') return;        // a square panel has no encoder: let the page scroll
    e.preventDefault();
    wheelAcc += e.deltaMode === 1 ? e.deltaY * 40 : e.deltaY;
    while (Math.abs(wheelAcc) >= 50) {
      const d = wheelAcc < 0 ? 1 : -1;    // scroll up = clockwise = warmer
      turn(d);
      wheelAcc += d * 50;
    }
  };
  screen.addEventListener('wheel', onWheel, { passive: false });
  ring.addEventListener('wheel', onWheel, { passive: false });

  const pt = (e) => {
    const r = screen.getBoundingClientRect();
    return [Math.round((e.clientX - r.left) / r.width * N), Math.round((e.clientY - r.top) / r.height * N)];
  };
  let touching = false;
  screen.addEventListener('pointerdown', (e) => { touching = true; screen.setPointerCapture(e.pointerId); api.touch(...pt(e), 1); });
  screen.addEventListener('pointermove', (e) => { if (touching) api.touch(...pt(e), 1); });
  const tup = (e) => { if (touching) { touching = false; api.touch(...pt(e), 0); } };
  screen.addEventListener('pointerup', tup);
  screen.addEventListener('pointercancel', tup);

  /* ---- square panel keys: real press-and-hold, debounced and repeated by ButtonInput ---- */
  const KEYMAP = { ArrowUp: 0, ArrowDown: 1, ' ': 2, Enter: 2 };
  const keyEls = (i) => document.querySelectorAll(`[data-key="${i}"]`);
  const held = new Set();
  const keyDown = (i) => { if (held.has(i)) return; held.add(i); keyEls(i).forEach((b) => b.classList.add('held')); api.key(i, 1); };
  const keyUp = (i) => { if (!held.delete(i)) return; keyEls(i).forEach((b) => b.classList.remove('held')); api.key(i, 0); };
  document.querySelectorAll('[data-key]').forEach((b) => {
    const i = +b.dataset.key;
    b.addEventListener('pointerdown', (e) => { b.setPointerCapture(e.pointerId); keyDown(i); });
    b.addEventListener('pointerup', () => keyUp(i));
    b.addEventListener('pointercancel', () => keyUp(i));
    b.addEventListener('keydown', (e) => { if (e.key === ' ' || e.key === 'Enter') e.preventDefault(); });
  });

  document.addEventListener('keydown', (e) => {
    if (e.target.closest('input, select, textarea')) return;
    if (kind === 'square') {
      const k = KEYMAP[e.key];
      if (k === undefined) return;
      if (k === 2 && e.target.tagName === 'BUTTON' && !e.target.dataset.key) return;   // let buttons work
      e.preventDefault();
      if (!e.repeat) keyDown(k);
      return;
    }
    if (e.key === 'ArrowRight') { turn(1); e.preventDefault(); }
    else if (e.key === 'ArrowLeft') { turn(-1); e.preventDefault(); }
    else if ((e.key === ' ' || e.key === 'Enter') && !e.repeat) {
      if (e.target.tagName === 'BUTTON' && e.target !== press) return;   // let buttons work
      down(); e.preventDefault();
    }
  });
  document.addEventListener('keyup', (e) => {
    if (kind === 'square') { if (KEYMAP[e.key] !== undefined) keyUp(KEYMAP[e.key]); return; }
    if (e.key === ' ' || e.key === 'Enter') up();
  });
  window.addEventListener('blur', () => { [0, 1, 2].forEach(keyUp); up(); });   // never leave a key stuck down

  /* ------------------------------------------------------------ speed */
  /* segmented controls: a glass thumb slides under the chosen button */
  function syncThumb(g) {
    const on = g.querySelector('button.on');
    let t = g.querySelector('.seg-thumb');
    if (!on) { if (t) t.remove(); return; }
    if (!t) { t = document.createElement('span'); t.className = 'seg-thumb'; g.prepend(t); }
    t.style.left = on.offsetLeft + 'px';
    t.style.width = on.offsetWidth + 'px';
  }
  const syncAllThumbs = () => document.querySelectorAll('.seg').forEach(syncThumb);
  /* re-measure whenever a control changes size (layout, web fonts arriving) */
  const segObs = new ResizeObserver((es) => es.forEach((e) => syncThumb(e.target)));
  document.querySelectorAll('.seg').forEach((g) => segObs.observe(g));
  if (document.fonts) document.fonts.ready.then(syncAllThumbs);
  function segment(id, attr, onPick) {
    const g = $(id);
    g.addEventListener('click', (e) => {
      const b = e.target.closest('button');
      if (!b) return;
      g.querySelectorAll('button').forEach((x) => x.classList.toggle('on', x === b));
      syncThumb(g);
      onPick(b.dataset[attr]);
    });
  }
  segment('speed', 'speed', (v) => api.speed(+v));

  /* ------------------------------------------------------------ device: round dial or square panel */
  const HINTS = {
    round: 'Drag the ring or scroll to turn · click the glass to touch · ← → Space',
    square: 'Hold a key to repeat · click the glass to touch · ↑ ↓ Space',
  };
  const setKind = (k) => {
    if (!(k in HINTS)) k = 'round';
    [0, 1, 2].forEach(keyUp);
    up();
    kind = k;
    api.layout(k === 'square' ? 1 : 0);   // the firmware's own layout swap (brew_ui_set_layout)
    $('device').closest('.stage').classList.toggle('square', k === 'square');
    document.querySelectorAll('#device-kind button').forEach((b) => b.classList.toggle('on', b.dataset.kind === k));
    syncThumb($('device-kind'));
    $('hint').textContent = HINTS[k];
    try { localStorage.setItem('brewdial.device', k); } catch (_) { /* private mode: not remembered */ }
  };
  segment('device-kind', 'kind', setKind);

  /* ------------------------------------------------------------ graph style (brew_ui_set_graph_style) */
  const setGraphStyle = (s) => {
    if (s !== 'mist') s = 'hairline';
    api.graphStyle(s === 'mist' ? 1 : 0);
    document.querySelectorAll('#graph-style button').forEach((b) => b.classList.toggle('on', b.dataset.style === s));
    syncThumb($('graph-style'));
    try { localStorage.setItem('brewdial.graph', s); } catch (_) { /* private mode: not remembered */ }
  };
  segment('graph-style', 'style', setGraphStyle);
  let savedKind = null;
  try { savedKind = localStorage.getItem('brewdial.device'); } catch (_) { /* ignore */ }
  setKind(qs.get('device') || savedKind || 'round');
  let savedGraph = null;
  try { savedGraph = localStorage.getItem('brewdial.graph'); } catch (_) { /* ignore */ }
  setGraphStyle(qs.get('graph') || savedGraph || 'hairline');
  requestAnimationFrame(syncAllThumbs);

  /* ------------------------------------------------------------ beer + probe */
  const beer = $('beer');
  let beerDragging = false;
  beer.addEventListener('pointerdown', () => { beerDragging = true; });
  beer.addEventListener('pointerup', () => { beerDragging = false; });
  beer.addEventListener('input', () => { api.beer(toC(+beer.value)); $('beer-v').textContent = (+beer.value).toFixed(1) + '°F'; });
  $('physics').onchange = (e) => api.physics(e.target.checked ? 1 : 0);

  /* ------------------------------------------------------------ fermenter (physical setup) */
  const GAL = 3.785;
  const PLANT_IDS = ['vol', 'room', 'loss', 'heatw', 'heatlag', 'coolw', 'coollag', 'yeast'];
  /* lags are entered in minutes. loss = W per degree C between beer and room */
  const PLANTS = {
    jacket:  { vol: 5, room: 68, loss: 1.8, heatw: 50, heatlag: 0, coolw: 150, coollag: 0, yeast: 0 },
    freezer: { vol: 5, room: 68, loss: 0.5, heatw: 25, heatlag: 0, coolw: 80, coollag: 0, yeast: 0 },
    bare:    { vol: 5, room: 68, loss: 3.0, heatw: 50, heatlag: 0, coolw: 150, coollag: 0, yeast: 0 },
    fast:    { vol: 5, room: 68, loss: 20, heatw: 3000, heatlag: 0, coolw: 3000, coollag: 0, yeast: 0 },
  };
  const INSULATION = { bare: 3.0, neoprene: 1.8, foam: 0.8, freezer: 0.5 };
  const plantNow = () => Object.fromEntries(PLANT_IDS.map((id) => [id, +$(id).value]));
  const pushPlant = () => {
    const p = plantNow();
    api.plant(p.vol * GAL, toC(p.room), p.loss, p.heatw, p.coolw, p.heatlag * 60, p.coollag * 60, p.yeast);
    const ins = Object.entries(INSULATION).find(([, v]) => Math.abs(v - p.loss) < 1e-6);
    $('insul').value = ins ? ins[0] : 'custom';
    explainPlant(p);
  };
  /* What the numbers mean in practice, at the room temperature. */
  const explainPlant = (p) => {
    const cap = p.vol * GAL * 4180;                       // J per degree C
    const fph = (w) => (w / cap * 3600 * 9 / 5);           // W -> F per hour
    const tau = p.loss > 0 ? cap / p.loss / 3600 : Infinity;
    const yeastRise = p.loss > 0 ? p.yeast / p.loss * 9 / 5 : Infinity;
    $('plant-explain').textContent =
      `Heating +${fph(p.heatw).toFixed(1)} °F/h · cooling −${fph(p.coolw).toFixed(1)} °F/h (at room temperature)
` +
      `Left alone: halfway to room temperature in ${(tau * Math.LN2).toFixed(1)} h` +
      (p.yeast ? ` · yeast alone holds it ${yeastRise.toFixed(1)} °F above room` : '');
  };
  $('plant-preset').onchange = (e) => {
    const p = PLANTS[e.target.value];
    if (!p) return;
    for (const id of PLANT_IDS) $(id).value = p[id];
    pushPlant();
  };
  $('insul').onchange = (e) => {
    if (e.target.value in INSULATION) { $('loss').value = INSULATION[e.target.value]; pushPlant(); }
  };
  for (const id of PLANT_IDS) $(id).onchange = () => { $('plant-preset').value = 'custom'; pushPlant(); };
  pushPlant();
  $('noise').onchange = (e) => api.noise(dF(+e.target.value));
  $('offset').onchange = (e) => api.offset(dF(+e.target.value));
  segment('fault', 'f', (v) => api.fault(+v));

  /* ------------------------------------------------------------ controller setup */
  /* times are entered in minutes, the API takes seconds */
  const SETUP = ['law', 'hdiff', 'cdiff', 'hon', 'hoff', 'con', 'coff', 'change'];
  const PRESETS = {
    // what the firmware ran before: +/-0.3 F deadband, no timing protection
    original:    { law: 0, hdiff: 0.3, cdiff: 0.3, hon: 0, hoff: 0, con: 0, coff: 0, change: 0 },
    // ChamberDefaults in lib/core/include/brew/control.h (what the boards run)
    recommended: { law: 1, hdiff: 0.5, cdiff: 0.5, hon: 0, hoff: 3, con: 0, coff: 3, change: 0 },
  };
  const applySetup = () => {
    const m = (id) => Math.round(+$(id).value * 60);
    api.setup(+$('law').value, +$('hdiff').value, +$('cdiff').value, m('hon'), m('hoff'), m('con'), m('coff'),
              m('change'), +$('histms').value);
  };
  for (const [name, p] of Object.entries(PRESETS)) {
    $('preset-' + name).onclick = () => { for (const id of SETUP) $(id).value = p[id]; applySetup(); };
  }
  $('apply').onclick = applySetup;
  $('reboot').onclick = () => api.reboot();
  $('factory').onclick = () => { if (confirm('Wipe the simulated controller\'s saved settings and reboot it?')) api.factory(); };

  /* ------------------------------------------------------------ link */
  const pushLink = () => api.link($('unplug').checked ? 1 : 0, +$('drop').value / 100, +$('corrupt').value / 100);
  segment('topology', 'split', (v) => { api.split(+v); $('link-opts').classList.toggle('off', v === '0'); });
  $('link-opts').classList.add('off');
  ['unplug', 'drop', 'corrupt'].forEach((id) => { $(id).onchange = pushLink; });

  /* ------------------------------------------------------------ status panels */
  const ACT = ['idle', 'cooling', 'heating'];
  const setText = (el, v) => { if (el.textContent !== v) el.textContent = v; };
  function render(s) {
    /* the room takes the dial's colour (same rules as brew_ui's compute_vs) */
    const vs = s.state.paused ? 'pause' : s.state.mode ? 'crash' : ['idle', 'cool', 'heat'][s.state.action];
    if (document.body.dataset.state !== vs) document.body.dataset.state = vs;

    setText($('simclock'), hms(s.sim_ms));
    const t = $('s-temp');
    setText(t, s.state.valid ? toF(s.state.temp_c).toFixed(1) : 'No probe');
    t.classList.toggle('invalid', !s.state.valid);
    setText($('s-sp'), toF(s.state.sp[s.state.mode]).toFixed(1));
    const a = $('s-action');
    setText(a, s.state.paused ? 'Paused' : ACT[s.state.action]);
    a.className = 'pill ' + (s.state.paused ? 'paused' : ACT[s.state.action]);
    setText($('s-extra'),
      `${s.state.mode ? 'Cold crash' : 'Ferment'} mode  ·  ferment ${fmtF(s.state.sp[0])}  ·  crash ${fmtF(s.state.sp[1])}  ·  ` +
      `units ${s.state.celsius ? '°C' : '°F'}  ·  boot ${s.boots}  ·  ${s.commits} settings writes`);

    for (const k of ['heat', 'cool']) {
      const r = s.relays[k];
      const el = $('r-' + k);
      el.classList.toggle('on', r.on);
      /* glow and meter = power the element actually delivers: a warm wrap keeps heating after its relay opens */
      const watts = s.plant[k + '_power_w'], maxW = s.plant[k + '_w'];
      el.style.setProperty('--pwr', maxW > 0 ? Math.min(1, watts / maxW).toFixed(3) : 0);
      const perH = s.sim_ms > 60000 ? `  ·  ${(r.switches / (s.sim_ms / 3600000)).toFixed(1)}/h` : '';
      setText(el.querySelector('.relay-state'), `${r.on ? 'On' : 'Off'} ${dur(r.in_state_ms)}`);
      setText(el.querySelector('.relay-sub'),
        `${watts.toFixed(0)} of ${maxW.toFixed(0)} W` +
        (r.wait_ms > 0 ? `  ·  ${k === 'cool' ? 'compressor rest' : 'held off'} ${dur(r.wait_ms)}` : '') +
        `\n${r.switches} switches${perH}  ·  on ${dur(r.on_ms)} total`);
    }

    if (!beerDragging && document.activeElement !== beer) {
      beer.value = toF(s.beer_c).toFixed(1);
      $('beer-v').textContent = toF(s.beer_c).toFixed(2) + '°F';
    }

    renderWifi(s.wifi);

    const L = s.link;
    $('link-stats').textContent = s.split
      ? `to controller ${L.bytes_to_ctl} B · to display ${L.bytes_to_ui} B · lost ${L.bytes_lost} B · ` +
        `flipped ${L.bytes_corrupted} B · bad frames ${L.bad_frames_display + L.bad_frames_controller}`
      : 'Same board: no cable (LocalBus).';
  }

  /* ------------------------------------------------------------ WiFi setup */
  /* The setup page is the device's own file (lib/network/web/setup.html), served by bd_http. It
   * runs in an iframe whose fetch() is routed here, so its /api calls reach the firmware's
   * handler exactly as they would over the setup network. */
  window.__bdFetch = (url, opt) => {
    const u = new URL(url, 'http://192.168.4.1/');
    const status = api.http(String(opt.method || 'GET').toUpperCase(), u.pathname, opt.body ? String(opt.body) : '');
    const body = status === 204 ? null : api.httpBody();
    return Promise.resolve(new Response(body, { status: status === 302 ? 200 : status, headers: { 'Content-Type': api.httpType() } }));
  };
  const wifiPage = $('wifi-page'), wifiOff = $('wifi-off');
  let pageShown = false;
  function showPage(on) {
    if (on === pageShown) return;
    pageShown = on;
    if (on) {
      api.http('GET', '/', '');
      const shim = '<script>window.fetch=function(u,o){return parent.__bdFetch(String(u),o||{});};<\/script>';
      wifiPage.srcdoc = api.httpBody().replace('<head>', '<head>' + shim);
    } else {
      wifiPage.removeAttribute('srcdoc');
    }
    wifiPage.hidden = !on;
    wifiOff.hidden = on;
  }
  $('wifi-phone').onchange = (e) => api.wifiPhone(e.target.checked ? 1 : 0);
  $('wifi-router').onchange = (e) => api.wifiRouter(e.target.checked ? 1 : 0);
  $('wifi-forget').onclick = () => api.wifiForget();
  if (qs.get('wifi') === 'new') api.wifiForget();       // start as a device never set up
  if (qs.get('wifi_phone') === '1') api.wifiPhone(1);   // for screenshots

  const mmss = (s) => `${Math.floor(s / 60)}:${String(s % 60).padStart(2, '0')}`;
  const WIFI_PHASE = { off: 'Off: nothing saved', hotspot: 'Setup network up', phone_joined: 'Phone on the setup network',
    connecting: 'Connecting', connected: 'Connected', offline: 'Not connected: retrying' };
  function renderWifi(w) {
    let t = WIFI_PHASE[w.phase] || w.phase;
    if (w.phase === 'connected' || w.phase === 'offline' || w.phase === 'connecting') t += `  ·  ${w.ssid}`;
    if (w.ap) t += `\nSetup network ${w.ap_ssid}  ·  password ${w.ap_password}  ·  closes in ${mmss(w.setup_left_s)}`;
    if (w.error) t += `\nLast try: ${w.error.replace('_', ' ')}`;
    setText($('wifi-state'), t);
    const ph = $('wifi-phone');
    if (ph.checked !== w.phone) ph.checked = w.phone;
    ph.disabled = !w.ap;
    $('wifi-router').checked = w.router;
    showPage(w.phone);
    if (!w.phone) {
      const html = w.ap
        ? `<strong>Phone not on the setup network</strong><span>Turn on \u201cPhone on the setup network\u201d, like scanning the code on the dial. The setup page opens here, at phone width.</span>`
        : `<strong>No setup network</strong><span>Setup is closed. Open it from the menu to change networks.</span>`;
      if (wifiOff.dataset.html !== html) { wifiOff.dataset.html = html; wifiOff.innerHTML = html; }
    }
  }

  /* ------------------------------------------------------------ log */
  const logEl = $('log');
  function drainLog() {
    let line, added = false;
    const showAct = $('log-actions').checked;
    while ((line = api.nextLog())) {
      const isAct = line.includes('] Controller: ');
      if (isAct && !showAct) continue;
      const li = document.createElement('li');
      const m = /^\[([^\]]*)\]\s?(.*)$/s.exec(line);
      const ts = document.createElement('span'), msg = document.createElement('span');
      ts.className = 't';
      msg.className = 'm';
      ts.textContent = m ? m[1] : '';
      msg.textContent = m ? m[2] : line;
      li.append(ts, msg);
      if (line.includes('UI -> controller')) li.className = 'cmd';
      else if (/INVALID|fault: (?!none)|unplugged/.test(line)) li.className = 'warn';
      else if (isAct) li.className = 'act' + (/heat/i.test(line) ? ' heat' : /cool/i.test(line) ? ' cool' : '');
      logEl.appendChild(li);
      added = true;
    }
    while (logEl.children.length > 400) logEl.firstChild.remove();
    if (added) logEl.scrollTop = logEl.scrollHeight;
  }
  $('log-clear').onclick = () => { logEl.textContent = ''; };

  /* ------------------------------------------------------------ chart */
  const chart = $('chart');
  const cctx = chart.getContext('2d');
  let pts = [];
  const MAX_PTS = 1500;
  $('chart-clear').onclick = () => { pts = []; };

  function sample(s) {
    if (pts.length && s.sim_ms === pts[pts.length - 1].t) return;   // paused
    pts.push({
      t: s.sim_ms, beer: toF(s.beer_c), probe: s.reading.valid ? toF(s.state.temp_c) : null,
      sp: toF(s.state.sp[s.state.mode]), act: s.state.action,
    });
    if (pts.length > MAX_PTS) pts.splice(0, pts.length - MAX_PTS);
  }

  function drawChart() {
    const dpr = window.devicePixelRatio || 1;
    const W = chart.clientWidth, H = chart.clientHeight;
    if (chart.width !== Math.round(W * dpr)) { chart.width = Math.round(W * dpr); chart.height = Math.round(H * dpr); }
    const c = cctx;
    c.setTransform(dpr, 0, 0, dpr, 0, 0);
    c.clearRect(0, 0, W, H);
    if (pts.length < 2) return;

    const t0 = pts[0].t, t1 = pts[pts.length - 1].t;
    let lo = Infinity, hi = -Infinity;
    for (const p of pts) for (const v of [p.beer, p.probe, p.sp]) if (v != null) { lo = Math.min(lo, v); hi = Math.max(hi, v); }
    const pad = Math.max(0.5, (hi - lo) * 0.12);
    lo -= pad; hi += pad;
    const L = 46, R = 14, T = 14, B = 26;
    const x = (t) => L + (t - t0) / Math.max(1, t1 - t0) * (W - L - R);
    const y = (v) => T + (1 - (v - lo) / (hi - lo)) * (H - T - B);
    const css = getComputedStyle(document.body);
    const col = (n) => css.getPropertyValue(n).trim();
    const NUM = '"Inter Tight", system-ui, sans-serif';
    /* CSS colour (#rrggbb or rgb()) with alpha; registered properties compute to rgb() */
    const alpha = (name, a) => {
      const v = col(name);
      let r, g, b;
      if (v[0] === '#') [r, g, b] = [1, 3, 5].map((i) => parseInt(v.slice(i, i + 2), 16));
      else [r, g, b] = (v.match(/[\d.]+/g) || [255, 255, 255]).map(Number);
      return `rgba(${r},${g},${b},${a})`;
    };

    // heating / cooling: soft columns that fade toward the floor, like the dial's graph fill
    for (const [act, name] of [[2, '--heat'], [1, '--cool']]) {
      const g = c.createLinearGradient(0, T, 0, H - B);
      g.addColorStop(0, alpha(name, 0));
      g.addColorStop(1, alpha(name, act === 2 ? 0.25 : 0.3));
      c.fillStyle = g;
      for (let i = 1; i < pts.length; i++) {
        if (pts[i - 1].act !== act) continue;
        c.fillRect(x(pts[i - 1].t), T, Math.max(1, x(pts[i].t) - x(pts[i - 1].t)), H - T - B);
      }
    }

    // grid + labels
    c.lineWidth = 1;
    c.font = `400 11px ${NUM}`;
    const step = niceStep((hi - lo) / 4);
    for (let v = Math.ceil(lo / step) * step; v <= hi; v += step) {
      c.strokeStyle = 'rgba(255,255,255,0.05)';
      c.beginPath(); c.moveTo(L, Math.round(y(v)) + 0.5); c.lineTo(W - R, Math.round(y(v)) + 0.5); c.stroke();
      c.fillStyle = col('--text-3');
      c.fillText(v.toFixed(step < 1 ? 1 : 0) + '°', 10, y(v) + 4);
    }
    c.fillStyle = col('--text-3');
    c.fillText(hms(t0), L, H - 8);
    const end = hms(t1);
    c.fillText(end, W - R - c.measureText(end).width, H - 8);
    setText($('chart-span'), `last ${dur(t1 - t0)} simulated`);

    const path = (key) => {
      c.beginPath();
      let pen = false;
      for (const p of pts) {
        if (p[key] == null) { pen = false; continue; }
        if (pen) c.lineTo(x(p.t), y(p[key])); else { c.moveTo(x(p.t), y(p[key])); pen = true; }
      }
    };
    // target: dotted white, as on the dial
    c.strokeStyle = 'rgba(255,255,255,0.38)'; c.lineWidth = 1.2; c.setLineDash([2, 5]); c.lineCap = 'round';
    path('sp'); c.stroke(); c.setLineDash([]);
    // probe reading: thin steel
    c.strokeStyle = col('--text-3'); c.lineWidth = 1;
    path('probe'); c.stroke();
    // beer: an area in the room's colour under a bright line
    const area = c.createLinearGradient(0, T, 0, H - B);
    area.addColorStop(0, alpha('--s-hi', 0.2));
    area.addColorStop(1, alpha('--s-hi', 0));
    path('beer');
    c.lineTo(x(t1), H - B); c.lineTo(x(t0), H - B); c.closePath();
    c.fillStyle = area; c.fill();
    c.save();
    c.shadowColor = col('--s-hi'); c.shadowBlur = 10;
    c.strokeStyle = '#f4f7fa'; c.lineWidth = 1.8; c.lineJoin = 'round';
    path('beer'); c.stroke();
    c.restore();
    // now: a dot at the newest point, like the dial's halo
    const last = pts[pts.length - 1];
    c.fillStyle = alpha('--s-hi', 0.25);
    c.beginPath(); c.arc(x(last.t), y(last.beer), 7, 0, Math.PI * 2); c.fill();
    c.fillStyle = '#fff';
    c.beginPath(); c.arc(x(last.t), y(last.beer), 3, 0, Math.PI * 2); c.fill();
  }

  function niceStep(raw) {
    const p = Math.pow(10, Math.floor(Math.log10(raw)));
    const n = raw / p;
    return (n < 1.5 ? 1 : n < 3.5 ? 2 : n < 7.5 ? 5 : 10) * p;
  }
}).catch((err) => {
  const l = $('loading');
  l.classList.add('err');
  l.textContent = 'The firmware failed to start:\n' + (err && err.stack || err);
});
