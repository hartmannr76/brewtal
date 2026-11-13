/* Headless check of the WebAssembly build: node sim/harness/web_smoke.js sim/build-web/web/brew_web.js
 * Loads the module exactly as the page does and runs knob -> controller -> relays. */
'use strict';
const path = require('path');
const create = require(path.resolve(process.argv[2] || 'sim/build-web/web/brew_web.js'));

create().then((M) => {
  const status = () => JSON.parse(M.UTF8ToString(M._bd_status()));
  const toF = (c) => c * 9 / 5 + 32;
  let now = 1000;
  const run = (ms) => { for (const end = now + ms; now < end;) M._bd_frame(now += 16); };
  let fails = 0;
  const check = (ok, what) => { console.log(`  [${ok ? 'ok' : 'FAIL'}] ${what}`); if (!ok) fails++; };

  M._bd_start(now);
  run(1000);
  let s = status();
  check(s.state.valid && Math.abs(toF(s.state.temp_c) - 68.4) < 0.2, 'boots with the beer at 68.4 F');

  const N = M._bd_size(), p = M._bd_rgba();
  let lit = 0;
  for (let i = 0; i < N * N * 4; i += 4) lit += M.HEAPU8[p + i] | M.HEAPU8[p + i + 1];
  check(lit > 0, 'screen rendered');

  M._bd_turn(6); run(200); M._bd_click(); run(500);
  s = status();
  check(Math.abs(toF(s.state.sp[0]) - 69) < 0.01 && s.state.action === 2 && s.relays.heat.on, 'knob set 69 F: heating');

  M._bd_set_split(1); run(4000);
  M._bd_turn(-4); run(200); M._bd_click(); run(1500);
  s = status();
  check(Math.abs(toF(s.state.sp[0]) - 67) < 0.01 && s.link.bytes_to_ctl > 0, 'split mode: command crossed the cable');

  /* square-panel keys: hold up ~0.7 s (step + 2 repeats), then select commits */
  const sp0 = toF(status().state.sp[0]);
  M._bd_key(0, 1); run(700); M._bd_key(0, 0); run(200);
  M._bd_key(2, 1); run(100); M._bd_key(2, 0); run(1500);
  s = status();
  check(Math.abs(toF(s.state.sp[0]) - (sp0 + 1.5)) < 0.05, `panel keys: held up then select (${sp0} -> ${toF(s.state.sp[0]).toFixed(2)} F)`);

  let line, n = 0;
  while ((line = M.UTF8ToString(M._bd_next_log()))) n++;
  check(n > 0, `event log has entries (${n})`);
  console.log(fails ? `FAILED (${fails})` : 'PASSED');
  process.exit(fails ? 1 : 0);
});
