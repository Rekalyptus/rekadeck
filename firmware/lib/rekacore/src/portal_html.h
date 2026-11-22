// The first-time setup page your phone sees after joining the Rekadeck's own Wi-Fi network. Both the ESP32 and
// the simulator serve this exact page: GET / (this), GET /scan (JSON list of networks), POST /save (the form).
// {{server}} is replaced with the default server address before serving.
#pragma once

namespace reka {

static const char PORTAL_HTML[] = R"HTML(<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1"><title>Set up your Rekadeck</title>
<style>
:root{--bg:#0f1012;--fg:#ececea;--soft:#9a9ea6;--line:#2b2d32;--accent:#86b98f}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--fg);font:16px/1.5 -apple-system,system-ui,sans-serif}
main{max-width:440px;margin:0 auto;padding:28px 18px 48px}h1{font-size:24px;margin:0 0 4px}p{color:var(--soft);margin:0 0 22px}
.net{display:flex;justify-content:space-between;width:100%;text-align:left;background:none;color:var(--fg);border:0;
 padding:11px 2px;font:inherit;cursor:pointer}.net span{color:var(--soft);font-size:14px}.net.sel{color:var(--accent)}
label{display:block;margin:22px 0 6px;color:var(--soft);font-size:14px}
details{margin-top:26px;color:var(--soft);font-size:14px}summary{cursor:pointer}details label{margin-top:12px}
input{width:100%;background:#17191c;color:var(--fg);border:1px solid var(--line);border-radius:10px;padding:12px;font:inherit}
button.go{margin-top:28px;width:100%;background:var(--accent);color:#0f1012;border:0;border-radius:10px;padding:13px;font:600 16px system-ui}
</style></head><body><main>
<h1>Set up your Rekadeck</h1><p>Pick your home Wi-Fi. The Rekadeck uses it to sync your cards.</p>
<div id="nets">Looking for networks...</div>
<form method="post" action="/save">
<label for="ssid">Network</label><input id="ssid" name="ssid" required>
<label for="pass">Password</label><input id="pass" name="pass" type="password">
<details><summary>Advanced</summary>
<label for="server">Server, only if you run your own</label><input id="server" name="server" value="{{server}}">
</details>
<button class="go" type="submit">Connect</button></form></main>
<script>
fetch('/scan').then(r=>r.json()).then(list=>{const el=document.getElementById('nets');el.innerHTML='';
 list.forEach(n=>{const b=document.createElement('button');b.type='button';b.className='net';
  b.innerHTML=n.ssid.replace(/</g,'&lt;')+'<span>'+(n.secure?'secured':'open')+'</span>';
  b.onclick=()=>{document.querySelectorAll('.net').forEach(x=>x.classList.remove('sel'));b.classList.add('sel');
   document.getElementById('ssid').value=n.ssid;document.getElementById('pass').focus()};el.appendChild(b)});
 if(!list.length)el.textContent='No networks found. Type yours below.'}).catch(()=>{});
</script></body></html>)HTML";

static const char PORTAL_SAVED_HTML[] = R"HTML(<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1"><title>Rekadeck</title>
<style>body{margin:0;background:#0f1012;color:#ececea;font:16px/1.5 -apple-system,system-ui,sans-serif}
main{max-width:440px;margin:0 auto;padding:28px 18px}p{color:#9a9ea6}</style></head><body><main>
<h1>Joining your Wi-Fi</h1><p>Watch the Rekadeck's screen. If the password was wrong it asks again, and this page
reloads when you rejoin its network.</p></main></body></html>)HTML";

}  // namespace reka
