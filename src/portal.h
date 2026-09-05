// ---------------------------------------------------------------------------
// portal.h  -  WiFi setup page, served only from the fallback hotspot when
// the board can't join the configured network.
// ---------------------------------------------------------------------------
#pragma once
#include <Arduino.h>

const char PORTAL_HTML[] PROGMEM = R"PAGE(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Contact Pressure Setup</title>
<style>
body{margin:0;font:15px/1.5 ui-sans-serif,system-ui,-apple-system,Segoe UI,Roboto,sans-serif;
     background:#0e1116;color:#e6edf3;display:flex;min-height:100vh;
     align-items:center;justify-content:center}
.card{background:#161b22;border:1px solid #2a313c;border-radius:10px;
      padding:24px;width:280px}
h1{font-size:16px;margin:0 0 16px}
label{display:block;font-size:12px;color:#8b949e;margin:12px 0 4px}
input{width:100%;box-sizing:border-box;background:#0d1117;color:#e6edf3;
      border:1px solid #2a313c;border-radius:6px;padding:8px 10px;font:inherit}
button{width:100%;margin-top:18px;background:#2f81f7;border:none;color:#fff;
       font-weight:600;padding:10px;border-radius:6px;font:inherit;cursor:pointer}
.hint{font-size:11px;color:#8b949e;margin-top:14px}
</style>
</head>
<body>
<div class="card">
  <h1>Connect Contact Pressure to WiFi</h1>
  <form action="/save" method="POST">
    <label>Network name (SSID)</label>
    <input name="ssid" required maxlength="32" autofocus>
    <label>Password</label>
    <input name="pass" type="password" maxlength="63">
    <button type="submit">Save &amp; reboot</button>
  </form>
  <p class="hint">The board will restart and try to join this network. If it
  fails again, this setup hotspot reopens automatically.</p>
</div>
</body>
</html>
)PAGE";
