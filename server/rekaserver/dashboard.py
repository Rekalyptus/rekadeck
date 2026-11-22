"""HTML for the mock server's dashboard and the device-link page."""

_STYLE = """
<style>
:root { --bg:#0e0f11; --fg:#e8e8e6; --soft:#9a9ea6; --line:#2a2c31; --accent:#7fb38a; --warn:#d9a35b; }
* { box-sizing:border-box; }
body { margin:0; background:var(--bg); color:var(--fg); font:15px/1.5 -apple-system, "Inter", "Segoe UI", sans-serif; }
main { max-width:1100px; margin:0 auto; padding:32px 20px 64px; }
h1 { font-size:26px; margin:0 0 4px; font-weight:700; }
h2 { font-size:17px; margin:36px 0 10px; font-weight:650; }
.sub { color:var(--soft); margin:0 0 24px; }
.row { display:flex; flex-wrap:wrap; gap:10px; align-items:center; }
button { background:#1c1e22; color:var(--fg); border:1px solid var(--line); border-radius:9px; padding:8px 14px;
         font:inherit; cursor:pointer; }
button:hover { border-color:#3b3e45; }
.stats { display:flex; flex-wrap:wrap; gap:28px; margin:18px 0 6px; }
.stat b { display:block; font-size:28px; font-weight:700; font-variant-numeric:tabular-nums; }
.stat span { color:var(--soft); font-size:13px; }
table { width:100%; border-collapse:collapse; font-variant-numeric:tabular-nums; }
td, th { text-align:left; padding:5px 12px 5px 0; vertical-align:top; }
th { color:var(--soft); font-weight:500; font-size:13px; }
td.q { max-width:430px; }
a { color:var(--accent); }
.soft { color:var(--soft); }
.due { color:var(--warn); }
.code { font:700 22px ui-monospace, Menlo, monospace; letter-spacing:2px; }
.wrap { overflow-x:auto; }
input { background:#16181b; color:var(--fg); border:1px solid var(--line); border-radius:9px; padding:10px 12px; font:inherit; }
.err { color:#e58a7f; }
</style>
"""

DASHBOARD_HTML = """<!doctype html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1">
<title>Rekalyptus mock server</title>""" + _STYLE + """</head><body><main>
<h1>Rekalyptus mock server</h1>
<p class="sub">Stands in for the real backend while the Rekadeck firmware is built. Content version <b id="ver">-</b>.</p>
<div class="row">
  <button onclick="act('/sim/phone_review?n=3')">Review 3 cards on my phone</button>
  <button onclick="act('/sim/add_card')">Add a card</button>
  <button onclick="act('/sim/make_due?n=5')">Make 5 more cards due</button>
  <a href="/link" target="_blank">Link a device</a>
  <button onclick="if(confirm('Reset all cards, reviews and devices?')) act('/sim/reset')">Reset</button>
</div>
<div class="stats">
  <div class="stat"><b id="dueNow">-</b><span>due now</span></div>
  <div class="stat"><b id="dueDevice">-</b><span>of them on the device</span></div>
  <div class="stat"><b id="dueToday">-</b><span>due in the next 12 h</span></div>
  <div class="stat"><b id="nrev">-</b><span>reviews received</span></div>
</div>
<div class="row">
  <span class="soft">Picture cards only the app can show: <b id="appOnly">-</b></span>
  <button onclick="setAppOnly(-1)">Fewer</button><button onclick="setAppOnly(1)">More</button>
</div>
<p class="soft" id="fw"></p>
<div id="codes"></div>
<h2>Devices</h2><div class="wrap"><table id="devs"></table></div>
<h2>Latest reviews</h2><div class="wrap"><table id="revs"></table></div>
<h2>Cards</h2><div class="wrap"><table id="cards"></table></div>
</main>
<script>
const fmt = ms => { const a = Math.abs(ms), s = ms < 0 ? '-' : ''; if (a < 3600e3) return s + Math.round(a/60e3) + 'm';
  if (a < 86400e3) return s + Math.round(a/3600e3) + 'h'; return s + (a/86400e3).toFixed(1) + 'd'; };
const ago = (now, t) => t ? fmt(now - t) + ' ago' : 'never';
const esc = s => String(s ?? '').replace(/[&<>]/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;'}[c]));
const G = ['skip', 'Again', 'Hard', 'Good', 'Easy'];
async function act(u) { await fetch(u, {method:'POST'}); load(); }
let appOnlyN = 0;
const setAppOnly = d => act('/sim/app_only?n=' + Math.max(0, appOnlyN + d));
async function load() {
  const s = await (await fetch('/sim/state')).json();
  ver.textContent = s.version; dueNow.textContent = s.due_now; dueToday.textContent = s.due_today;
  dueDevice.textContent = s.due_device; appOnly.textContent = appOnlyN = s.app_only;
  fw.textContent = !s.firmware_dir ? 'No firmware updates offered (start the server with REKASIM_FIRMWARE_DIR to offer some).'
    : s.releases.length ? 'Firmware releases in ' + s.firmware_dir + ': ' + s.releases.map(r => r.version + ' ' + r.channel).join(', ')
    : 'No valid firmware releases in ' + s.firmware_dir + ' (the server log says why).';
  nrev.textContent = s.reviews.length ? s.reviews[0].id : 0;
  codes.innerHTML = s.codes.map(c => `<p>A device is waiting to be linked with code <span class="code">${c.user_code}</span>
     <a href="/link?code=${c.user_code}" target="_blank">link it</a></p>`).join('');
  devs.innerHTML = '<tr><th>Device</th><th>Last seen</th><th>Battery</th><th>Batch</th><th>Cards left</th><th>Unsynced</th><th>Clock drift</th><th>Firmware</th></tr>' +
    (s.devices.map(d => `<tr><td>${esc(d.name)} <span class="soft">${d.id}</span></td><td>${ago(s.now, d.last_seen_ms)}</td>
     <td>${d.battery == null ? '-' : Math.round(d.battery) + '%'}${d.charging ? ' charging' : ''}</td>
     <td>${d.batch_id || '-'} <span class="${d.batch_version == s.version ? 'soft' : 'due'}">v${d.batch_version ?? '-'}${d.batch_version == s.version ? '' : ' stale'}</span></td>
     <td>${d.cards_left ?? '-'}</td><td>${d.unsynced ?? '-'}</td>
     <td>${d.drift == null ? '-' : (d.drift*100).toFixed(1) + '%'}</td><td>${d.fw || '-'}</td></tr>`).join('') || '<tr><td class="soft">None linked yet</td></tr>');
  revs.innerHTML = '<tr><th>When</th><th>Card</th><th>Grade</th><th>From</th><th>To reveal</th></tr>' +
    s.reviews.map(r => `<tr><td>${ago(s.now, r.graded_ms)}</td><td>${r.card_id}</td><td>${G[r.grade]}${r.choice >= 0 ? ' (picked ' + 'ABCD'[r.choice] + ')' : ''}</td>
     <td>${r.source}</td><td>${r.revealed_ms && r.shown_ms ? ((r.revealed_ms - r.shown_ms)/1000).toFixed(1) + ' s' : '-'}</td></tr>`).join('');
  cards.innerHTML = '<tr><th>Card</th><th>Front</th><th>Due</th><th>Stability</th><th>Reps</th><th>Preview</th></tr>' +
    s.cards.map(c => `<tr><td>${c.id}</td><td class="q">${esc(c.front)}</td>
     <td class="${c.due_in_ms <= 0 ? 'due' : 'soft'}">${c.due_in_ms <= 0 ? 'due ' + fmt(-c.due_in_ms) + ' ago' : 'in ' + fmt(c.due_in_ms)}</td>
     <td>${c.s == null ? 'new' : c.s.toFixed(1) + 'd'}</td><td>${c.reps}</td>
     <td><a href="/preview/${c.id}.png?view=q" target="_blank">question</a> <a href="/preview/${c.id}.png" target="_blank">answer</a></td></tr>`).join('');
}
load(); setInterval(load, 2000);
</script></body></html>"""

LINK_HTML = """<!doctype html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1">
<title>Link a Rekadeck</title>""" + _STYLE + """</head><body><main style="max-width:520px">
<h1>Link a Rekadeck</h1>
<p class="sub">Enter the code shown on the device's screen.</p>
<!--err-->
<form method="post" class="row" style="flex-direction:column;align-items:stretch;gap:14px">
  <input name="code" value="{{code}}" placeholder="ABC-123" autocomplete="off" class="code" style="text-transform:uppercase">
  <input name="name" value="Rekadeck" placeholder="Device name">
  <button type="submit">Link</button>
</form></main></body></html>"""

LINKED_HTML = """<!doctype html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1">
<title>Rekadeck linked</title>""" + _STYLE + """</head><body><main style="max-width:520px">
<h1>{{name}} is linked</h1>
<p class="sub">The device picks this up within a few seconds and downloads its first cards.</p>
<p><a href="/">Back to the dashboard</a></p></main></body></html>"""
