#include "web_ui.hpp"
#include "board.hpp"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace note4c::idf {
namespace {
constexpr const char* kTag = "web_ui";

// Minimal self-contained JSON builder and recursive-descent parser (no external components).
std::string json_escape(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 2);
    out.push_back('"');
    for (unsigned char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            } else {
                out.push_back(static_cast<char>(c));
            }
        }
    }
    out.push_back('"');
    return out;
}

struct JsonVal {
    enum class Type { Null, Bool, Number, String, Array, Object } type = Type::Null;
    bool b = false;
    double n = 0.0;
    std::string s;
    std::vector<JsonVal> arr;
    std::map<std::string, JsonVal> obj;

    const JsonVal* get(const std::string& k) const {
        if (type != Type::Object) return nullptr;
        const auto it = obj.find(k);
        return (it == obj.end()) ? nullptr : &it->second;
    }
};

class JsonParser {
public:
    explicit JsonParser(std::string_view src) : src_(src) {}
    bool parse(JsonVal& out) {
        skip_ws();
        if (!parse_value(out, 0)) return false;
        skip_ws();
        return pos_ == src_.size();
    }

private:
    void skip_ws() {
        while (pos_ < src_.size() && std::isspace(static_cast<unsigned char>(src_[pos_]))) ++pos_;
    }

    bool parse_value(JsonVal& out, int depth) {
        if (depth > 16) return false;
        skip_ws();
        if (pos_ >= src_.size()) return false;
        const char c = src_[pos_];
        if (c == '{') return parse_object(out, depth + 1);
        if (c == '[') return parse_array(out, depth + 1);
        if (c == '"') { out.type = JsonVal::Type::String; return parse_string(out.s); }
        if (c == 't' && src_.substr(pos_, 4) == "true") { pos_ += 4; out.type = JsonVal::Type::Bool; out.b = true; return true; }
        if (c == 'f' && src_.substr(pos_, 5) == "false") { pos_ += 5; out.type = JsonVal::Type::Bool; out.b = false; return true; }
        if (c == 'n' && src_.substr(pos_, 4) == "null") { pos_ += 4; out.type = JsonVal::Type::Null; return true; }
        if (c == '-' || std::isdigit(static_cast<unsigned char>(c))) return parse_number(out);
        return false;
    }

    bool parse_string(std::string& out) {
        if (pos_ >= src_.size() || src_[pos_] != '"') return false;
        ++pos_;
        out.clear();
        while (pos_ < src_.size()) {
            const char c = src_[pos_++];
            if (c == '"') return true;
            if (c == '\\') {
                if (pos_ >= src_.size()) return false;
                const char e = src_[pos_++];
                switch (e) {
                case '"': case '\\': case '/': out.push_back(e); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    if (pos_ + 4 > src_.size()) return false;
                    unsigned code = 0;
                    for (int i = 0; i < 4; ++i) {
                        const char h = src_[pos_++];
                        code <<= 4;
                        if (h >= '0' && h <= '9') code |= static_cast<unsigned>(h - '0');
                        else if (h >= 'a' && h <= 'f') code |= static_cast<unsigned>(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') code |= static_cast<unsigned>(h - 'A' + 10);
                        else return false;
                    }
                    if (code < 0x80) out.push_back(static_cast<char>(code));
                    break;
                }
                default: return false;
                }
            } else {
                out.push_back(c);
            }
        }
        return false;
    }

    bool parse_number(JsonVal& out) {
        const size_t start = pos_;
        if (src_[pos_] == '-') ++pos_;
        if (pos_ >= src_.size() || !std::isdigit(static_cast<unsigned char>(src_[pos_]))) return false;
        while (pos_ < src_.size() && std::isdigit(static_cast<unsigned char>(src_[pos_]))) ++pos_;
        if (pos_ < src_.size() && src_[pos_] == '.') {
            ++pos_;
            while (pos_ < src_.size() && std::isdigit(static_cast<unsigned char>(src_[pos_]))) ++pos_;
        }
        if (pos_ < src_.size() && (src_[pos_] == 'e' || src_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < src_.size() && (src_[pos_] == '+' || src_[pos_] == '-')) ++pos_;
            while (pos_ < src_.size() && std::isdigit(static_cast<unsigned char>(src_[pos_]))) ++pos_;
        }
        std::string tmp(src_.substr(start, pos_ - start));
        out.type = JsonVal::Type::Number;
        out.n = std::strtod(tmp.c_str(), nullptr);
        return true;
    }

    bool parse_array(JsonVal& out, int depth) {
        ++pos_;
        out.type = JsonVal::Type::Array;
        skip_ws();
        if (pos_ < src_.size() && src_[pos_] == ']') { ++pos_; return true; }
        while (pos_ < src_.size()) {
            JsonVal item;
            if (!parse_value(item, depth)) return false;
            out.arr.push_back(std::move(item));
            skip_ws();
            if (pos_ < src_.size() && src_[pos_] == ',') { ++pos_; continue; }
            if (pos_ < src_.size() && src_[pos_] == ']') { ++pos_; return true; }
            return false;
        }
        return false;
    }

    bool parse_object(JsonVal& out, int depth) {
        ++pos_;
        out.type = JsonVal::Type::Object;
        skip_ws();
        if (pos_ < src_.size() && src_[pos_] == '}') { ++pos_; return true; }
        while (pos_ < src_.size()) {
            skip_ws();
            std::string key;
            if (!parse_string(key)) return false;
            skip_ws();
            if (pos_ >= src_.size() || src_[pos_] != ':') return false;
            ++pos_;
            JsonVal val;
            if (!parse_value(val, depth)) return false;
            out.obj[std::move(key)] = std::move(val);
            skip_ws();
            if (pos_ < src_.size() && src_[pos_] == ',') { ++pos_; continue; }
            if (pos_ < src_.size() && src_[pos_] == '}') { ++pos_; return true; }
            return false;
        }
        return false;
    }

    std::string_view src_;
    size_t pos_ = 0;
};

// Self-contained mobile-friendly calendar management UI (no external CDN dependencies).
const char kIndexHtml[] = R"HTML(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>NOTE4C Calendar Manager</title>
<style>
:root{--bg:#f5f4ef;--card:#fff;--ink:#18181b;--muted:#52525b;--line:#e4e4e7;--red:#c82323;--yel:#f0c419;--blk:#18181b}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--ink);font:15px/1.45 -apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,sans-serif;padding:16px}
.wrap{max-width:680px;margin:0 auto}
header{background:var(--blk);color:#fff;padding:16px 18px;border-radius:12px;border-bottom:4px solid var(--red);margin-bottom:16px;display:flex;justify-content:space-between;align-items:center;gap:12px}
header h1{margin:0;font-size:19px}
.badge{background:var(--yel);color:var(--blk);padding:3px 9px;border-radius:999px;font-size:12px;font-weight:700}
.card{background:var(--card);border:1px solid var(--line);border-radius:12px;padding:16px;margin-bottom:14px;box-shadow:0 1px 2px rgba(0,0,0,.04)}
.card h2{margin:0 0 12px;font-size:16px;display:flex;justify-content:space-between;align-items:center}
label{display:block;font-size:13px;font-weight:600;color:var(--muted);margin:10px 0 4px}
input[type=text],input[type=password],input[type=url],input[type=time],select{width:100%;padding:9px 11px;border:1px solid #d4d4d8;border-radius:8px;font:inherit;background:#fff}
.row{display:flex;gap:10px;align-items:center;flex-wrap:wrap}
.row>*{flex:1;min-width:140px}
.btn{border:0;border-radius:8px;padding:10px 14px;font:600 14px/1 inherit;cursor:pointer;display:inline-flex;align-items:center;justify-content:center;gap:6px}
.btn-primary{background:var(--blk);color:#fff}
.btn-accent{background:var(--red);color:#fff}
.btn-yel{background:var(--yel);color:var(--blk)}
.btn-ghost{background:#f4f4f5;color:var(--ink);border:1px solid var(--line)}
.btn-sm{padding:6px 10px;font-size:13px}
.cal-item{border:1px solid var(--line);border-radius:10px;padding:12px;margin-bottom:10px;background:#fafaf9}
.cal-top{display:flex;gap:8px;align-items:center;margin-bottom:8px;flex-wrap:wrap}
.swatches{display:inline-flex;gap:6px;align-items:center}
.sw{width:26px;height:26px;border-radius:50%;border:2px solid #d4d4d8;cursor:pointer;position:relative}
.sw[data-c=black]{background:var(--blk)}
.sw[data-c=red]{background:var(--red)}
.sw[data-c=yellow]{background:var(--yel)}
.sw.active{outline:2px solid var(--ink);outline-offset:2px}
.actions{display:flex;gap:10px;flex-wrap:wrap;margin-top:8px}
.actions .btn{flex:1;min-width:140px}
.status{padding:10px 12px;border-radius:8px;background:#f4f4f5;font-size:13px;margin-top:10px;white-space:pre-wrap}
.status.ok{background:#ecfdf5;color:#065f46;border:1px solid #a7f3d0}
.status.err{background:#fef2f2;color:#991b1b;border:1px solid #fecaca}
.help{font-size:13px;color:var(--muted);margin-top:8px}
details summary{cursor:pointer;font-size:13px;color:var(--muted);font-weight:600}
</style>
</head>
<body>
<div class="wrap">
  <header>
    <h1>NOTE4C Calendar</h1>
    <div style="display:flex;gap:6px;align-items:center;flex-wrap:wrap">
      <a href="#otaCard" class="badge" id="updBadge" style="display:none;background:var(--red);color:#fff;text-decoration:none">Update!</a>
      <span class="badge" id="batBadge" style="display:none"></span>
      <span class="badge" id="modeBadge">Loading...</span>
    </div>
  </header>

  <div id="updBanner" class="card" style="display:none;background:#fefce8;border:2px solid var(--red);padding:12px 14px">
    <div style="display:flex;justify-content:space-between;align-items:center;gap:10px;flex-wrap:wrap">
      <div>
        <div style="font-weight:700;color:var(--red)" id="updTitle">New firmware update available!</div>
        <div class="help" style="margin:2px 0 0;color:var(--ink)" id="updDetail"></div>
      </div>
      <div style="display:flex;gap:8px;align-items:center">
        <button class="btn btn-accent btn-sm" onclick="jumpAndInstallOta()">Update Now</button>
        <button class="btn btn-ghost btn-sm" onclick="document.getElementById('updBanner').style.display='none'" title="Dismiss">&times;</button>
      </div>
    </div>
  </div>

  <div class="card">
    <h2>Wi-Fi Connection <button class="btn btn-ghost btn-sm" onclick="scanWifi()">Scan</button></h2>
    <div style="display:flex;align-items:center;justify-content:space-between;gap:8px;background:#fafaf9;border:1px solid var(--line);border-radius:8px;padding:7px 10px;margin-bottom:10px;font-size:13px">
      <span>Wi-Fi MAC: <b id="macAddr" style="font-family:ui-monospace,SFMono-Regular,Menlo,monospace">...</b></span>
      <button type="button" class="btn btn-ghost btn-sm" onclick="copyMac()">Copy</button>
    </div>
    <div id="scanList" style="display:none;margin-bottom:8px">
      <label>Nearby 2.4 GHz Networks</label>
      <select id="wifiPick" onchange="if(this.value)document.getElementById('ssid').value=this.value"></select>
    </div>
    <div class="row">
      <div>
        <label for="ssid">Wi-Fi Name (SSID)</label>
        <input id="ssid" type="text" maxlength="32" placeholder="Your 2.4 GHz Wi-Fi">
      </div>
      <div>
        <label for="pass">Wi-Fi Password</label>
        <input id="pass" type="password" maxlength="63" placeholder="Password">
      </div>
    </div>
  </div>

  <div class="card">
    <h2>Schedule &amp; Timezone</h2>
    <div class="row">
      <div>
        <label for="tzPreset">Timezone</label>
        <select id="tzPreset" onchange="onTzPreset()">
          <option value="PDT7">Vancouver / BC (Permanent UTC-7, No DST)</option>
          <option value="MST7">Yukon / Arizona (UTC-7, No DST)</option>
          <option value="PST8PDT,M3.2.0,M11.1.0">US Pacific - LA / Seattle (DST)</option>
          <option value="MST7MDT,M3.2.0,M11.1.0">US Mountain - Denver / Calgary (DST)</option>
          <option value="CST6CDT,M3.2.0,M11.1.0">US Central - Chicago (DST)</option>
          <option value="EST5EDT,M3.2.0,M11.1.0">US Eastern - New York / Toronto (DST)</option>
          <option value="GMT0BST,M3.5.0/1,M10.5.0">UK (London)</option>
          <option value="CET-1CEST,M3.5.0,M10.5.0/3">Central Europe (Paris / Berlin)</option>
          <option value="CST-8">China / HK / Singapore / Taipei</option>
          <option value="JST-9">Japan (Tokyo)</option>
          <option value="AEST-10AEDT,M10.1.0,M4.1.0/3">Australia (Sydney)</option>
          <option value="UTC0">UTC</option>
          <option value="__custom">Custom POSIX TZ...</option>
        </select>
      </div>
      <div>
        <label for="syncInterval">Sync Frequency</label>
        <select id="syncInterval" onchange="updateScheduleHint()">
          <option value="30">Every 30 minutes</option>
          <option value="60">Every 1 hour</option>
          <option value="120">Every 2 hours</option>
          <option value="240">Every 4 hours</option>
          <option value="360">Every 6 hours</option>
          <option value="720">Every 12 hours</option>
          <option value="1440">Once a day</option>
        </select>
      </div>
      <div>
        <label for="syncTime">Start / Sync Time</label>
        <input id="syncTime" type="time" value="05:00" onchange="updateScheduleHint()">
      </div>
    </div>
    <p class="help" id="schedHint" style="margin:6px 0 0"></p>
    <div id="customTzWrap" style="display:none">
      <label for="tzCustom">Custom POSIX TZ String</label>
      <input id="tzCustom" type="text" maxlength="64" placeholder="MST7MDT,M3.2.0,M11.1.0">
    </div>
  </div>

  <div class="card">
    <h2>Subscribed Calendars <button class="btn btn-yel btn-sm" onclick="addCal()">+ Add Calendar</button></h2>
    <div id="calList"></div>
    <details>
      <summary>Where to find your .ics calendar link</summary>
      <p class="help">
        <b>Google Calendar:</b> Settings &rarr; click your calendar &rarr; <i>Secret address in iCal format</i>.<br>
        <b>Apple iCloud:</b> Calendar app / icloud.com &rarr; Share icon &rarr; turn on <i>Public Calendar</i> &rarr; copy <code>webcal://</code> link.<br>
        <b>Outlook / M365:</b> Settings &rarr; Calendar &rarr; Shared calendars &rarr; Publish a calendar (ICS link).
      </p>
    </details>
  </div>

  <div class="card">
    <div class="actions">
      <button class="btn btn-primary" onclick="saveConfig(false)">Save Settings</button>
      <button class="btn btn-accent" onclick="saveConfig(true)">Save &amp; Sync Now</button>
      <button class="btn btn-ghost" onclick="finishSleep()">Finish &amp; Sleep</button>
    </div>
    <div id="statusBox" class="status">Ready.</div>
  </div>

  <div class="card" id="otaCard">
    <h2>Firmware Update (OTA) <span class="badge" id="fwBadge">...</span></h2>
    <div id="fwCurrent" style="background:#fafaf9;border:1px solid var(--line);border-radius:8px;padding:8px 10px;margin-bottom:10px;font-size:13px">
      Installed: <b id="fwInstalled">...</b>
    </div>
    <label for="ghRepo">GitHub Releases Update</label>
    <div class="row" style="margin-bottom:6px">
      <input id="ghRepo" type="text" value="ckwcfm/note4c" maxlength="100" placeholder="owner/repo">
      <button class="btn btn-ghost" id="ghCheckBtn" onclick="checkGithubOta(false)" style="flex:0 0 auto">Check Update</button>
      <button class="btn btn-accent" id="ghInstallBtn" onclick="installGithubOta()" disabled style="flex:0 0 auto;opacity:.5">Install Update</button>
    </div>
    <label style="display:inline-flex;align-items:center;gap:6px;font-weight:400;margin:2px 0 6px;cursor:pointer">
      <input type="checkbox" id="ghForce" onchange="refreshInstallBtn()"> Allow reinstalling current/older version
    </label>
    <details style="margin-top:8px">
      <summary>Or upload a local .bin file manually</summary>
      <p class="help">Upload a compiled <code>note4c_framework.bin</code> over Wi-Fi. Wi-Fi and calendar settings are preserved.</p>
      <div class="row" style="align-items:stretch;margin-top:6px">
        <input id="otaFile" type="file" accept=".bin,application/octet-stream" style="padding:7px;border:1px solid #d4d4d8;border-radius:8px;background:#fafaf9">
        <button class="btn btn-primary" id="otaBtn" onclick="uploadOta()" style="flex:0 0 auto">Upload &amp; Update</button>
      </div>
    </details>
    <div id="otaBarWrap" style="display:none;margin-top:10px;background:#e4e4e7;border-radius:999px;height:10px;overflow:hidden">
      <div id="otaBar" style="height:100%;width:0%;background:var(--red);transition:width .15s"></div>
    </div>
    <div id="otaStatus" class="status" style="display:none"></div>
  </div>
</div>

<script>
let cals = [];
let passSaved = false;

function setStatus(msg, cls) {
  const el = document.getElementById('statusBox');
  el.textContent = msg;
  el.className = 'status' + (cls ? ' ' + cls : '');
}

function onTzPreset() {
  const sel = document.getElementById('tzPreset');
  document.getElementById('customTzWrap').style.display = sel.value === '__custom' ? 'block' : 'none';
}

function getTzValue() {
  const sel = document.getElementById('tzPreset');
  return sel.value === '__custom' ? document.getElementById('tzCustom').value.trim() : sel.value;
}

function setTzValue(tz) {
  const sel = document.getElementById('tzPreset');
  let found = false;
  for (const o of sel.options) {
    if (o.value === tz) { sel.value = tz; found = true; break; }
  }
  if (!found) {
    sel.value = '__custom';
    document.getElementById('tzCustom').value = tz || 'UTC0';
  }
  onTzPreset();
}

function renderCals() {
  const box = document.getElementById('calList');
  if (!cals.length) {
    box.innerHTML = '<p class="help">No calendars yet. Click <b>+ Add Calendar</b> to subscribe to an .ics feed.</p>';
    return;
  }
  box.innerHTML = '';
  cals.forEach((c, idx) => {
    const div = document.createElement('div');
    div.className = 'cal-item';
    div.innerHTML = `
      <div class="cal-top">
        <input type="checkbox" ${c.enabled ? 'checked' : ''} onchange="cals[${idx}].enabled=this.checked" title="Enable">
        <input type="text" maxlength="24" value="${esc(c.name)}" placeholder="Calendar name (e.g. Work)" oninput="cals[${idx}].name=this.value" style="flex:1;min-width:120px">
        <div class="swatches" title="Display color">
          <button type="button" class="sw ${c.color==='black'?'active':''}" data-c="black" onclick="setCol(${idx},'black')" title="Black"></button>
          <button type="button" class="sw ${c.color==='red'?'active':''}" data-c="red" onclick="setCol(${idx},'red')" title="Red"></button>
          <button type="button" class="sw ${c.color==='yellow'?'active':''}" data-c="yellow" onclick="setCol(${idx},'yellow')" title="Yellow"></button>
        </div>
        <button type="button" class="btn btn-ghost btn-sm" onclick="delCal(${idx})">Remove</button>
      </div>
      <input type="url" maxlength="320" value="${esc(c.url)}" placeholder="https://... or webcal://... (.ics URL)" oninput="cals[${idx}].url=this.value">
    `;
    box.appendChild(div);
  });
}

function esc(s) { return (s||'').replace(/&/g,'&amp;').replace(/"/g,'&quot;').replace(/</g,'&lt;'); }
function setCol(i, col) { cals[i].color = col; renderCals(); }
function delCal(i) { cals.splice(i, 1); renderCals(); }
function addCal() {
  if (cals.length >= 8) { setStatus('Maximum 8 calendars supported.', 'err'); return; }
  const cols = ['red','yellow','black'];
  cals.push({ name: '', url: '', color: cols[cals.length % 3], enabled: true });
  renderCals();
}

function updateScheduleHint() {
  const iv = parseInt(document.getElementById('syncInterval').value || '1440', 10);
  const t = (document.getElementById('syncTime').value || '05:00').split(':');
  const ah = parseInt(t[0] || '5', 10), am = parseInt(t[1] || '0', 10);
  const el = document.getElementById('schedHint');
  if (iv >= 1440) {
    el.textContent = 'Syncs once a day at ' + String(ah).padStart(2,'0') + ':' + String(am).padStart(2,'0') + '.';
    return;
  }
  const slots = [];
  const start = (ah * 60 + am) % iv;
  for (let m = start; m < 1440 && slots.length < 6; m += iv) {
    slots.push(String(Math.floor(m / 60)).padStart(2,'0') + ':' + String(m % 60).padStart(2,'0'));
  }
  const label = iv < 60 ? (iv + ' min') : ((iv / 60) + (iv === 60 ? ' hour' : ' hours'));
  el.textContent = 'Syncs every ' + label + ' (' + slots.join(', ') + (1440 / iv > 6 ? ', ...' : '') + ').';
}

let currentFwVer = '';
let latestGhAsset = '';
let latestGhTag = '';
let latestGhCmp = 0;

function parseSemver(v) {
  const m = String(v || '').trim().replace(/^v/i, '').match(/^(\d+)(?:\.(\d+))?(?:\.(\d+))?/);
  if (!m) return null;
  return [parseInt(m[1] || '0', 10), parseInt(m[2] || '0', 10), parseInt(m[3] || '0', 10)];
}

function compareSemver(a, b) {
  const pa = parseSemver(a), pb = parseSemver(b);
  if (!pa || !pb) return 0;
  for (let i = 0; i < 3; ++i) {
    if (pa[i] !== pb[i]) return pa[i] > pb[i] ? 1 : -1;
  }
  return 0;
}

function refreshInstallBtn() {
  const btn = document.getElementById('ghInstallBtn');
  const force = document.getElementById('ghForce').checked;
  const canInstall = !!latestGhAsset && (latestGhCmp > 0 || force);
  btn.disabled = !canInstall;
  btn.style.opacity = canInstall ? '1' : '.5';
  btn.textContent = latestGhCmp > 0 ? ('Install ' + latestGhTag) : (force ? ('Reinstall ' + (latestGhTag || 'Latest')) : 'Up to date');
}

function updateBadges(d) {
  document.getElementById('modeBadge').textContent = d.mode + ' (' + d.ip + ')';
  if (d.fw_version) {
    currentFwVer = d.fw_version;
    const vStr = currentFwVer.startsWith('v') ? currentFwVer : ('v' + currentFwVer);
    document.getElementById('fwBadge').textContent = vStr + (d.fw_slot ? (' · ' + d.fw_slot) : '');
    const built = d.fw_build_date ? (' · built ' + d.fw_build_date) : '';
    document.getElementById('fwInstalled').textContent = vStr + (d.fw_slot ? (' (slot ' + d.fw_slot + ')') : '') + built;
  }
  const bb = document.getElementById('batBadge');
  if (typeof d.battery_percent === 'number' && d.battery_percent >= 0) {
    const v = d.battery_mv ? (' ' + (d.battery_mv / 1000).toFixed(2) + 'V') : '';
    bb.textContent = 'Bat ' + d.battery_percent + '%' + (d.charging ? ' CHG' : '') + v;
    bb.style.display = 'inline-block';
  }
}

async function loadAll() {
  try {
    const r = await fetch('/api/config');
    const d = await r.json();
    updateBadges(d);
    document.getElementById('macAddr').textContent = d.mac || 'Unknown';
    document.getElementById('ssid').value = d.wifi_ssid || '';
    passSaved = !!d.wifi_password_set;
    document.getElementById('pass').placeholder = passSaved ? 'Saved (leave blank to keep)' : 'Wi-Fi password';
    setTzValue(d.timezone || 'PDT7');
    const hh = String(d.sync_hour ?? 5).padStart(2,'0');
    const mm = String(d.sync_minute ?? 0).padStart(2,'0');
    document.getElementById('syncTime').value = hh + ':' + mm;
    const ivSel = document.getElementById('syncInterval');
    const ivVal = String(d.sync_interval_minutes ?? 1440);
    ivSel.value = [...ivSel.options].some(o => o.value === ivVal) ? ivVal : '1440';
    updateScheduleHint();
    cals = (d.calendars || []).map(x => ({ name: x.name||'', url: x.url||'', color: x.color||'black', enabled: x.enabled!==false }));
    renderCals();
    showSyncSummary(d);
    if (d.mode === 'Wi-Fi') checkGithubOta(true);
  } catch (e) {
    setStatus('Failed to load settings: ' + e, 'err');
  }
}

function showSyncSummary(d) {
  updateBadges(d);
  if (d.syncing) { setStatus('Syncing calendars in background & updating e-paper display...', ''); return; }
  if (d.last_sync && d.last_sync.ran) {
    const lines = ['Last sync: ' + d.last_sync.date + ' ' + d.last_sync.time + ' (' + d.last_sync.today_total + ' today, ' + d.last_sync.tomorrow_total + ' tomorrow, ' + (d.last_sync.day3_total ?? 0) + ' day 3)'];
    for (const c of (d.last_sync.calendars || [])) {
      lines.push(' - ' + c.name + ': ' + (c.ok ? (c.today + ' / ' + c.tomorrow + ' / ' + (c.day3 ?? 0) + ' events (3d)') : ('ERROR: ' + c.error)));
    }
    setStatus(lines.join('\n'), d.last_sync.errors > 0 ? 'err' : 'ok');
  } else if (d.status_message) {
    setStatus(d.status_message, '');
  }
}

async function scanWifi() {
  setStatus('Scanning nearby 2.4 GHz Wi-Fi networks...', '');
  try {
    const r = await fetch('/api/scan');
    const list = await r.json();
    const sel = document.getElementById('wifiPick');
    sel.innerHTML = '<option value="">-- Select a Wi-Fi network --</option>';
    for (const n of list) {
      const o = document.createElement('option');
      o.value = n.ssid;
      o.textContent = n.ssid + ' (' + n.rssi + ' dBm' + (n.open ? ', open' : '') + ')';
      sel.appendChild(o);
    }
    document.getElementById('scanList').style.display = 'block';
    setStatus('Found ' + list.length + ' Wi-Fi networks.', 'ok');
  } catch (e) {
    setStatus('Scan failed: ' + e, 'err');
  }
}

async function saveConfig(andSync) {
  const t = (document.getElementById('syncTime').value || '05:00').split(':');
  const payload = {
    wifi_ssid: document.getElementById('ssid').value.trim(),
    wifi_password: document.getElementById('pass').value,
    keep_password: passSaved && document.getElementById('pass').value === '',
    timezone: getTzValue(),
    sync_hour: parseInt(t[0] || '5', 10),
    sync_minute: parseInt(t[1] || '0', 10),
    sync_interval_minutes: parseInt(document.getElementById('syncInterval').value || '1440', 10),
    calendars: cals.filter(c => c.url.trim().length > 0).map(c => ({
      name: c.name.trim() || 'Calendar',
      url: c.url.trim(),
      color: c.color,
      enabled: !!c.enabled
    }))
  };
  setStatus('Saving settings...', '');
  try {
    const r = await fetch('/api/config', {
      method: 'POST',
      headers: {'Content-Type': 'application/json'},
      body: JSON.stringify(payload)
    });
    const res = await r.json();
    if (!r.ok || !res.ok) { setStatus('Save failed: ' + (res.error || r.status), 'err'); return; }
    passSaved = !!res.wifi_password_set;
    document.getElementById('pass').value = '';
    document.getElementById('pass').placeholder = passSaved ? 'Saved (leave blank to keep)' : 'Wi-Fi password';
    if (res.reconnecting) {
      setStatus('Saved! NOTE4C is connecting to your Wi-Fi and syncing your calendars now. Check the NOTE4C screen.', 'ok');
      return;
    }
    if (andSync) {
      await fetch('/api/sync', { method: 'POST' });
      setStatus('Saved! Syncing calendars & refreshing screen (takes ~15-25s)...', '');
      pollStatus();
    } else {
      setStatus('Settings saved.', 'ok');
    }
  } catch (e) {
    setStatus('Save error: ' + e, 'err');
  }
}

async function pollStatus() {
  for (let i = 0; i < 45; ++i) {
    await new Promise(r => setTimeout(r, 1500));
    try {
      const r = await fetch('/api/status');
      const d = await r.json();
      showSyncSummary(d);
      if (!d.syncing) return;
    } catch (_) { return; }
  }
}

function copyMac() {
  const mac = document.getElementById('macAddr').textContent.trim();
  if (!mac) return;
  if (navigator.clipboard && window.isSecureContext) {
    navigator.clipboard.writeText(mac).then(() => setStatus('Copied MAC: ' + mac, 'ok'));
  } else {
    const ta = document.createElement('textarea');
    ta.value = mac; ta.style.position = 'fixed'; ta.style.opacity = '0';
    document.body.appendChild(ta); ta.select();
    try { document.execCommand('copy'); setStatus('Copied MAC: ' + mac, 'ok'); } catch (_) {}
    document.body.removeChild(ta);
  }
}

async function finishSleep() {
  setStatus('Exiting manage mode and entering daily battery sleep...', 'ok');
  try { await fetch('/api/sleep', { method: 'POST' }); } catch (_) {}
}

function setOtaStatus(msg, cls) {
  const el = document.getElementById('otaStatus');
  el.style.display = 'block';
  el.textContent = msg;
  el.className = 'status' + (cls ? ' ' + cls : '');
}

function jumpAndInstallOta() {
  document.getElementById('otaCard').scrollIntoView({ behavior: 'smooth' });
  installGithubOta();
}

function setUpdateNotice(show, latest, cur, kb) {
  const banner = document.getElementById('updBanner');
  const badge = document.getElementById('updBadge');
  if (show) {
    document.getElementById('updTitle').textContent = 'Update available: ' + latest;
    document.getElementById('updDetail').textContent = 'Installed: ' + cur + ' -> Latest: ' + latest + kb;
    banner.style.display = 'block';
    badge.textContent = 'Update ' + latest;
    badge.style.display = 'inline-block';
  } else {
    banner.style.display = 'none';
    badge.style.display = 'none';
  }
}

async function checkGithubOta(silent) {
  const repo = document.getElementById('ghRepo').value.trim() || 'ckwcfm/note4c';
  if (!silent) setOtaStatus('Checking GitHub Releases for ' + repo + '...', '');
  try {
    const r = await fetch('/api/ota-check?repo=' + encodeURIComponent(repo));
    const d = await r.json();
    if (!r.ok || !d.ok) {
      latestGhAsset = '';
      latestGhTag = '';
      latestGhCmp = 0;
      refreshInstallBtn();
      setUpdateNotice(false);
      if (!silent) setOtaStatus('Check failed: ' + (d.error || r.status), 'err');
      return;
    }
    latestGhTag = d.tag || '';
    latestGhAsset = d.asset_url || '';
    latestGhCmp = compareSemver(latestGhTag, currentFwVer);
    refreshInstallBtn();
    const curLabel = currentFwVer.startsWith('v') ? currentFwVer : ('v' + currentFwVer);
    const kb = d.size ? (' (' + (d.size / 1024).toFixed(1) + ' KB)') : '';
    if (latestGhCmp > 0) {
      setUpdateNotice(true, latestGhTag, curLabel, kb);
      setOtaStatus('Update available: ' + latestGhTag + kb + ' (installed: ' + curLabel + '). Click Install to update.', 'ok');
    } else if (latestGhCmp === 0) {
      setUpdateNotice(false);
      setOtaStatus('Up to date! You are already running ' + curLabel + '.', 'ok');
    } else {
      setUpdateNotice(false);
      setOtaStatus('Installed firmware (' + curLabel + ') is newer than latest GitHub release (' + latestGhTag + ').', 'ok');
    }
  } catch (e) {
    if (!silent) setOtaStatus('Check error: ' + e, 'err');
  }
}

async function installGithubOta() {
  const repo = document.getElementById('ghRepo').value.trim() || 'ckwcfm/note4c';
  const label = latestGhTag ? (latestGhTag + ' from ' + repo) : ('latest release from ' + repo);
  if (!confirm('Download and install ' + label + ' over Wi-Fi and reboot NOTE4C?')) return;
  document.getElementById('ghInstallBtn').disabled = true;
  document.getElementById('otaBarWrap').style.display = 'block';
  document.getElementById('otaBar').style.width = '5%';
  setOtaStatus('Starting GitHub OTA download (do not unplug power)...', '');
  try {
    const r = await fetch('/api/ota-github', {
      method: 'POST',
      headers: {'Content-Type': 'application/json'},
      body: JSON.stringify({ repo: repo, url: latestGhAsset })
    });
    const res = await r.json();
    if (!r.ok || !res.ok) {
      document.getElementById('ghInstallBtn').disabled = false;
      setOtaStatus('OTA start failed: ' + (res.error || r.status), 'err');
      return;
    }
    pollOtaStatus();
  } catch (e) {
    document.getElementById('ghInstallBtn').disabled = false;
    setOtaStatus('OTA error: ' + e, 'err');
  }
}

async function pollOtaStatus() {
  for (let i = 0; i < 120; ++i) {
    await new Promise(r => setTimeout(r, 1000));
    try {
      const r = await fetch('/api/status');
      const d = await r.json();
      if (d.ota_total > 0) {
        const pct = Math.min(100, Math.round((d.ota_written / d.ota_total) * 100));
        document.getElementById('otaBar').style.width = pct + '%';
        setOtaStatus((d.ota_status || 'Downloading...') + ' ' + pct + '% (' + Math.round(d.ota_written / 1024) + ' / ' + Math.round(d.ota_total / 1024) + ' KB)', '');
      } else if (d.ota_status) {
        setOtaStatus(d.ota_status, d.ota_failed ? 'err' : (d.ota_reboot ? 'ok' : ''));
      }
      if (d.ota_reboot) {
        document.getElementById('otaBar').style.width = '100%';
        setOtaStatus(d.ota_status || 'OTA complete! Rebooting into new firmware...', 'ok');
        return;
      }
      if (!d.ota_active && d.ota_failed) {
        document.getElementById('ghInstallBtn').disabled = false;
        setOtaStatus(d.ota_status || 'GitHub OTA failed.', 'err');
        return;
      }
    } catch (_) {
      setOtaStatus('NOTE4C is rebooting into the new firmware! Refresh in ~10 seconds.', 'ok');
      return;
    }
  }
  document.getElementById('ghInstallBtn').disabled = false;
}

function uploadOta() {
  const input = document.getElementById('otaFile');
  if (!input.files || !input.files.length) {
    setOtaStatus('Please choose a note4c_framework.bin file first.', 'err');
    return;
  }
  const file = input.files[0];
  if (file.size < 65536 || file.size > 4100000) {
    setOtaStatus('Invalid firmware file size (' + file.size + ' bytes).', 'err');
    return;
  }
  if (!confirm('Upload ' + file.name + ' (' + (file.size / 1024).toFixed(1) + ' KB) and reboot NOTE4C?')) return;
  const btn = document.getElementById('otaBtn');
  btn.disabled = true;
  document.getElementById('otaBarWrap').style.display = 'block';
  document.getElementById('otaBar').style.width = '0%';
  setOtaStatus('Uploading firmware (do not unplug power)...', '');

  const xhr = new XMLHttpRequest();
  xhr.open('POST', '/api/ota', true);
  xhr.setRequestHeader('Content-Type', 'application/octet-stream');
  xhr.upload.onprogress = (ev) => {
    if (ev.lengthComputable && ev.total > 0) {
      const pct = Math.min(100, Math.round((ev.loaded / ev.total) * 100));
      document.getElementById('otaBar').style.width = pct + '%';
      setOtaStatus('Uploading: ' + pct + '% (' + Math.round(ev.loaded / 1024) + ' / ' + Math.round(ev.total / 1024) + ' KB)...', '');
    }
  };
  xhr.onload = () => {
    btn.disabled = false;
    try {
      const res = JSON.parse(xhr.responseText || '{}');
      if (xhr.status === 200 && res.ok) {
        document.getElementById('otaBar').style.width = '100%';
        setOtaStatus('Verified & written to ' + res.slot + '! NOTE4C is rebooting into the new firmware now.', 'ok');
      } else {
        setOtaStatus('OTA failed: ' + (res.error || ('HTTP ' + xhr.status)), 'err');
      }
    } catch (_) {
      setOtaStatus('OTA response error (HTTP ' + xhr.status + ').', 'err');
    }
  };
  xhr.onerror = () => {
    btn.disabled = false;
    setOtaStatus('Network error during upload. Device still runs previous firmware.', 'err');
  };
  xhr.send(file);
}

loadAll();
</script>
</body>
</html>
)HTML";

esp_err_t send_json(httpd_req_t* req, const std::string& json, const char* status = "200 OK") {
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, json.c_str(), static_cast<ssize_t>(json.size()));
}

} // namespace

WebUiServer::WebUiServer(CalendarStorage& storage, WifiPortal& wifi,
                         WebRuntimeState& state, SemaphoreHandle_t mutex)
    : storage_(storage), wifi_(wifi), state_(state), mutex_(mutex) {}

WebUiServer::~WebUiServer() {
    stop();
}

void WebUiServer::touch_activity() {
    had_http_activity_.store(true, std::memory_order_release);
    last_activity_ms_.store(now_ms(), std::memory_order_release);
}

bool WebUiServer::take_github_ota_request(std::string& out_repo, std::string& out_url) {
    if (!github_ota_requested_.exchange(false, std::memory_order_acq_rel)) return false;
    xSemaphoreTake(mutex_, portMAX_DELAY);
    out_repo = gh_ota_repo_;
    out_url = gh_ota_url_;
    xSemaphoreGive(mutex_);
    return true;
}

bool WebUiServer::start() {
    if (server_) return true;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 14;
    cfg.stack_size = 10240;
    cfg.lru_purge_enable = true;
    if (httpd_start(&server_, &cfg) != ESP_OK) {
        server_ = nullptr;
        return false;
    }

    auto reg = [this](const char* uri, httpd_method_t method, esp_err_t (*fn)(httpd_req_t*)) {
        httpd_uri_t u{};
        u.uri = uri;
        u.method = method;
        u.handler = fn;
        u.user_ctx = this;
        httpd_register_uri_handler(server_, &u);
    };

    reg("/", HTTP_GET, handle_index);
    reg("/api/config", HTTP_GET, handle_get_config);
    reg("/api/config", HTTP_POST, handle_post_config);
    reg("/api/scan", HTTP_GET, handle_scan);
    reg("/api/sync", HTTP_POST, handle_sync);
    reg("/api/status", HTTP_GET, handle_status);
    reg("/api/sleep", HTTP_POST, handle_sleep);
    reg("/api/ota", HTTP_POST, handle_ota);
    reg("/api/ota-check", HTTP_GET, handle_ota_check);
    reg("/api/ota-github", HTTP_POST, handle_ota_github);
    httpd_register_err_handler(server_, HTTPD_404_NOT_FOUND, handle_404);

    had_http_activity_.store(false, std::memory_order_release);
    last_activity_ms_.store(now_ms(), std::memory_order_release);
    ESP_LOGI(kTag, "HTTP management server started on port %d", cfg.server_port);
    return true;
}

void WebUiServer::stop() {
    if (!server_) return;
    httpd_stop(server_);
    server_ = nullptr;
    ESP_LOGI(kTag, "HTTP management server stopped");
}

std::string WebUiServer::build_status_json(bool include_config) {
    std::string out;
    out.reserve(1024);
    xSemaphoreTake(mutex_, portMAX_DELAY);

    out += "{\"mode\":";
    out += json_escape(state_.mode_label);
    out += ",\"ip\":";
    out += json_escape(state_.ip_address);
    out += ",\"mac\":";
    out += json_escape(state_.mac_address);
    out += ",\"fw_version\":";
    out += json_escape(state_.fw_version);
    out += ",\"fw_build_date\":";
    out += json_escape(state_.fw_build_date);
    out += ",\"fw_slot\":";
    out += json_escape(state_.fw_slot);
    out += ",\"battery_percent\":";
    out += std::to_string(state_.battery_percent);
    out += ",\"battery_mv\":";
    out += std::to_string(state_.battery_mv);
    out += ",\"charging\":";
    out += state_.charging ? "true" : "false";
    out += ",\"syncing\":";
    out += state_.syncing ? "true" : "false";
    out += ",\"ota_active\":";
    out += ota_in_progress() ? "true" : "false";
    out += ",\"ota_written\":";
    out += std::to_string(state_.ota_progress.written.load(std::memory_order_acquire));
    out += ",\"ota_total\":";
    out += std::to_string(state_.ota_progress.total.load(std::memory_order_acquire));
    out += ",\"ota_status\":";
    out += json_escape(state_.ota_status);
    out += ",\"ota_failed\":";
    out += state_.ota_failed ? "true" : "false";
    out += ",\"ota_reboot\":";
    out += state_.ota_reboot ? "true" : "false";
    out += ",\"status_message\":";
    out += json_escape(state_.status_message);

    if (include_config) {
        out += ",\"wifi_ssid\":";
        out += json_escape(state_.config.wifi_ssid);
        out += ",\"wifi_password_set\":";
        out += !state_.config.wifi_password.empty() ? "true" : "false";
        out += ",\"timezone\":";
        out += json_escape(state_.config.timezone);
        out += ",\"sync_hour\":";
        out += std::to_string(state_.config.sync_hour);
        out += ",\"sync_minute\":";
        out += std::to_string(state_.config.sync_minute);
        out += ",\"sync_interval_minutes\":";
        out += std::to_string(state_.config.sync_interval_minutes);
        out += ",\"calendars\":[";
        for (size_t i = 0; i < state_.config.calendars.size(); ++i) {
            if (i > 0) out.push_back(',');
            const auto& c = state_.config.calendars[i];
            out += "{\"name\":";
            out += json_escape(c.name);
            out += ",\"url\":";
            out += json_escape(c.url);
            out += ",\"color\":";
            out += json_escape(calendar_color_name(c.color));
            out += ",\"enabled\":";
            out += c.enabled ? "true" : "false";
            out.push_back('}');
        }
        out.push_back(']');
    }

    out += ",\"last_sync\":{\"ran\":";
    out += state_.has_synced ? "true" : "false";
    if (state_.has_synced) {
        char dbuf[16], tbuf[16];
        std::snprintf(dbuf, sizeof(dbuf), "%04d-%02d-%02d",
                      state_.last_sync.today.year, state_.last_sync.today.month, state_.last_sync.today.day);
        std::snprintf(tbuf, sizeof(tbuf), "%02d:%02d",
                      state_.last_sync.local_hour, state_.last_sync.local_minute);
        out += ",\"date\":";
        out += json_escape(dbuf);
        out += ",\"time\":";
        out += json_escape(tbuf);
        out += ",\"today_total\":";
        out += std::to_string(state_.last_sync.days[0].size());
        out += ",\"tomorrow_total\":";
        out += std::to_string(state_.last_sync.days[1].size());
        out += ",\"day3_total\":";
        out += std::to_string(state_.last_sync.days[2].size());
        out += ",\"errors\":";
        out += std::to_string(state_.last_sync.error_count);
        out += ",\"calendars\":[";
        for (size_t i = 0; i < state_.last_sync.calendars.size(); ++i) {
            if (i > 0) out.push_back(',');
            const auto& cs = state_.last_sync.calendars[i];
            out += "{\"name\":";
            out += json_escape(cs.name);
            out += ",\"ok\":";
            out += cs.ok ? "true" : "false";
            out += ",\"today\":";
            out += std::to_string(cs.day_counts[0]);
            out += ",\"tomorrow\":";
            out += std::to_string(cs.day_counts[1]);
            out += ",\"day3\":";
            out += std::to_string(cs.day_counts[2]);
            out += ",\"error\":";
            out += json_escape(cs.error);
            out.push_back('}');
        }
        out.push_back(']');
    }
    out += "}}";

    xSemaphoreGive(mutex_);
    return out;
}

esp_err_t WebUiServer::handle_index(httpd_req_t* req) {
    auto* self = static_cast<WebUiServer*>(req->user_ctx);
    self->touch_activity();
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, kIndexHtml, HTTPD_RESP_USE_STRLEN);
}

esp_err_t WebUiServer::handle_get_config(httpd_req_t* req) {
    auto* self = static_cast<WebUiServer*>(req->user_ctx);
    self->touch_activity();
    return send_json(req, self->build_status_json(true));
}

esp_err_t WebUiServer::handle_status(httpd_req_t* req) {
    auto* self = static_cast<WebUiServer*>(req->user_ctx);
    self->touch_activity();
    return send_json(req, self->build_status_json(false));
}

esp_err_t WebUiServer::handle_scan(httpd_req_t* req) {
    auto* self = static_cast<WebUiServer*>(req->user_ctx);
    self->touch_activity();
    const auto nets = self->wifi_.scan_networks();
    std::string out = "[";
    for (size_t i = 0; i < nets.size(); ++i) {
        if (i > 0) out.push_back(',');
        out += "{\"ssid\":";
        out += json_escape(nets[i].ssid);
        out += ",\"rssi\":";
        out += std::to_string(nets[i].rssi);
        out += ",\"open\":";
        out += nets[i].open ? "true" : "false";
        out.push_back('}');
    }
    out.push_back(']');
    return send_json(req, out);
}

esp_err_t WebUiServer::handle_post_config(httpd_req_t* req) {
    auto* self = static_cast<WebUiServer*>(req->user_ctx);
    self->touch_activity();
    if (req->content_len <= 0 || req->content_len > 8192) {
        return send_json(req, R"({"ok":false,"error":"Body too large or empty"})", "400 Bad Request");
    }
    std::string body(static_cast<size_t>(req->content_len), '\0');
    int received = 0;
    while (received < req->content_len) {
        const int r = httpd_req_recv(req, body.data() + received, req->content_len - received);
        if (r <= 0) {
            if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
            return send_json(req, R"({"ok":false,"error":"Receive failed"})", "400 Bad Request");
        }
        received += r;
    }

    JsonVal root;
    JsonParser parser(body);
    if (!parser.parse(root) || root.type != JsonVal::Type::Object) {
        return send_json(req, R"({"ok":false,"error":"Invalid JSON"})", "400 Bad Request");
    }

    xSemaphoreTake(self->mutex_, portMAX_DELAY);
    CalendarConfig updated = self->state_.config;
    const std::string prev_ssid = updated.wifi_ssid;
    const std::string prev_pass = updated.wifi_password;
    const bool was_ap = self->state_.is_ap_mode;

    if (const auto* j = root.get("wifi_ssid"); j && j->type == JsonVal::Type::String) {
        updated.wifi_ssid = j->s;
    }
    const auto* kp = root.get("keep_password");
    const bool keep_pw = (kp && kp->type == JsonVal::Type::Bool && kp->b);
    if (const auto* j = root.get("wifi_password"); j && j->type == JsonVal::Type::String) {
        if (!keep_pw || !j->s.empty()) {
            updated.wifi_password = j->s;
        }
    }
    if (const auto* j = root.get("timezone"); j && j->type == JsonVal::Type::String) {
        updated.timezone = j->s;
    }
    if (const auto* j = root.get("sync_hour"); j && j->type == JsonVal::Type::Number) {
        updated.sync_hour = static_cast<uint8_t>(j->n);
    }
    if (const auto* j = root.get("sync_minute"); j && j->type == JsonVal::Type::Number) {
        updated.sync_minute = static_cast<uint8_t>(j->n);
    }
    if (const auto* j = root.get("sync_interval_minutes"); j && j->type == JsonVal::Type::Number) {
        updated.sync_interval_minutes = static_cast<uint16_t>(j->n);
    }
    if (const auto* arr = root.get("calendars"); arr && arr->type == JsonVal::Type::Array) {
        updated.calendars.clear();
        for (const auto& item : arr->arr) {
            if (item.type != JsonVal::Type::Object || updated.calendars.size() >= CalendarConfig::kMaxCalendars) break;
            CalendarSub sub;
            if (const auto* f = item.get("name"); f && f->type == JsonVal::Type::String) sub.name = f->s;
            if (const auto* f = item.get("url"); f && f->type == JsonVal::Type::String) sub.url = f->s;
            if (const auto* f = item.get("color"); f && f->type == JsonVal::Type::String) sub.color = parse_calendar_color(f->s);
            if (const auto* f = item.get("enabled"); f && f->type == JsonVal::Type::Bool) sub.enabled = f->b;
            if (!sub.url.empty()) updated.calendars.push_back(std::move(sub));
        }
    }

    CalendarConfig::sanitize(updated);
    const bool saved = self->storage_.save(updated);
    if (saved) self->state_.config = updated;
    const bool wifi_diff = (updated.wifi_ssid != prev_ssid || updated.wifi_password != prev_pass);
    const bool will_reconnect = saved && updated.has_wifi() && (was_ap || wifi_diff);
    const bool pw_set = !updated.wifi_password.empty();
    xSemaphoreGive(self->mutex_);

    if (!saved) {
        return send_json(req, R"({"ok":false,"error":"Failed to save to NVS"})", "500 Internal Server Error");
    }
    if (will_reconnect) {
        self->wifi_changed_.store(true, std::memory_order_release);
    }

    char resp[128];
    std::snprintf(resp, sizeof(resp),
                  R"({"ok":true,"wifi_password_set":%s,"reconnecting":%s})",
                  pw_set ? "true" : "false", will_reconnect ? "true" : "false");
    return send_json(req, resp);
}

esp_err_t WebUiServer::handle_sync(httpd_req_t* req) {
    auto* self = static_cast<WebUiServer*>(req->user_ctx);
    self->touch_activity();
    self->sync_requested_.store(true, std::memory_order_release);
    return send_json(req, R"({"ok":true})");
}

esp_err_t WebUiServer::handle_sleep(httpd_req_t* req) {
    auto* self = static_cast<WebUiServer*>(req->user_ctx);
    self->touch_activity();
    self->sleep_requested_.store(true, std::memory_order_release);
    return send_json(req, R"({"ok":true})");
}

esp_err_t WebUiServer::handle_ota(httpd_req_t* req) {
    auto* self = static_cast<WebUiServer*>(req->user_ctx);
    self->touch_activity();

    xSemaphoreTake(self->mutex_, portMAX_DELAY);
    const bool busy_sync = self->state_.syncing;
    xSemaphoreGive(self->mutex_);
    if (busy_sync) {
        return send_json(req, R"({"ok":false,"error":"Calendar sync in progress; retry in a few seconds"})", "409 Conflict");
    }
    if (self->ota_active_.exchange(true, std::memory_order_acq_rel)) {
        return send_json(req, R"({"ok":false,"error":"OTA already in progress"})", "409 Conflict");
    }

    struct OtaGuard {
        std::atomic<bool>& flag;
        esp_ota_handle_t handle = 0;
        bool started = false;
        ~OtaGuard() {
            if (started) esp_ota_abort(handle);
            flag.store(false, std::memory_order_release);
        }
    } guard{self->ota_active_};

    const esp_partition_t* update = esp_ota_get_next_update_partition(nullptr);
    if (!update) {
        return send_json(req, R"({"ok":false,"error":"No inactive OTA partition available"})", "500 Internal Server Error");
    }
    if (req->content_len <= 0 || static_cast<size_t>(req->content_len) < sizeof(esp_app_desc_t) + 32 ||
        static_cast<size_t>(req->content_len) > update->size) {
        return send_json(req, R"({"ok":false,"error":"Firmware size out of range for OTA slot"})", "400 Bad Request");
    }

    ESP_LOGI(kTag, "Starting OTA write to '%s' @ 0x%08lx (%d bytes)",
             update->label, static_cast<unsigned long>(update->address), req->content_len);

    std::vector<uint8_t> buf(4096);
    std::vector<uint8_t> header_buf;
    header_buf.reserve(512);
    constexpr size_t kMinHeaderBytes = 32 + sizeof(esp_app_desc_t);
    int remaining = req->content_len;
    size_t total_written = 0;
    bool header_checked = false;

    while (remaining > 0) {
        const int to_read = std::min<int>(remaining, static_cast<int>(buf.size()));
        const int r = httpd_req_recv(req, reinterpret_cast<char*>(buf.data()), to_read);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (r <= 0) {
            ESP_LOGE(kTag, "OTA recv interrupted after %u bytes", static_cast<unsigned>(total_written));
            return send_json(req, R"({"ok":false,"error":"Upload interrupted; previous firmware untouched"})", "400 Bad Request");
        }
        self->touch_activity();
        remaining -= r;

        if (!header_checked) {
            header_buf.insert(header_buf.end(), buf.data(), buf.data() + r);
            if (header_buf.size() < kMinHeaderBytes) {
                continue;
            }
            const uint8_t magic = header_buf[0];
            const uint16_t chip_id = static_cast<uint16_t>(header_buf[12]) |
                                     static_cast<uint16_t>(static_cast<uint16_t>(header_buf[13]) << 8);
            esp_app_desc_t desc{};
            std::memcpy(&desc, header_buf.data() + 32, sizeof(desc));
            if (magic != 0xE9 || chip_id != 9 || desc.magic_word != ESP_APP_DESC_MAGIC_WORD) {
                return send_json(req, R"({"ok":false,"error":"Not a valid ESP32-S3 application image"})", "400 Bad Request");
            }
            if (std::strncmp(desc.project_name, "note4c_framework", sizeof(desc.project_name)) != 0) {
                return send_json(req, R"J({"ok":false,"error":"Rejected: image is not note4c_framework (NOTE4C only)"})J", "400 Bad Request");
            }

            const esp_err_t berr = esp_ota_begin(update, OTA_WITH_SEQUENTIAL_WRITES, &guard.handle);
            if (berr != ESP_OK) {
                ESP_LOGE(kTag, "esp_ota_begin failed: %s", esp_err_to_name(berr));
                return send_json(req, R"({"ok":false,"error":"Failed to begin OTA flash write"})", "500 Internal Server Error");
            }
            guard.started = true;
            header_checked = true;

            const esp_err_t werr = esp_ota_write(guard.handle, header_buf.data(), header_buf.size());
            if (werr != ESP_OK) {
                ESP_LOGE(kTag, "esp_ota_write failed: %s", esp_err_to_name(werr));
                return send_json(req, R"({"ok":false,"error":"Flash write error; previous firmware untouched"})", "500 Internal Server Error");
            }
            total_written += header_buf.size();
            header_buf.clear();
            continue;
        }

        const esp_err_t werr = esp_ota_write(guard.handle, buf.data(), static_cast<size_t>(r));
        if (werr != ESP_OK) {
            ESP_LOGE(kTag, "esp_ota_write failed: %s", esp_err_to_name(werr));
            return send_json(req, R"({"ok":false,"error":"Flash write error; previous firmware untouched"})", "500 Internal Server Error");
        }
        total_written += static_cast<size_t>(r);
    }

    if (!header_checked) {
        return send_json(req, R"({"ok":false,"error":"Upload too small to be a valid firmware image"})", "400 Bad Request");
    }

    guard.started = false; // esp_ota_end consumes handle whether it succeeds or fails
    const esp_err_t eerr = esp_ota_end(guard.handle);
    if (eerr != ESP_OK) {
        ESP_LOGE(kTag, "esp_ota_end validation failed: %s", esp_err_to_name(eerr));
        return send_json(req, R"({"ok":false,"error":"Firmware image verification failed; previous firmware kept"})", "400 Bad Request");
    }

    const esp_err_t serr = esp_ota_set_boot_partition(update);
    if (serr != ESP_OK) {
        ESP_LOGE(kTag, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(serr));
        return send_json(req, R"({"ok":false,"error":"Failed to activate new OTA slot"})", "500 Internal Server Error");
    }

    ESP_LOGI(kTag, "OTA complete (%u bytes) -> next boot slot '%s'",
             static_cast<unsigned>(total_written), update->label);
    self->reboot_requested_.store(true, std::memory_order_release);

    char resp[160];
    std::snprintf(resp, sizeof(resp),
                  R"({"ok":true,"slot":"%s","bytes":%u,"rebooting":true})",
                  update->label, static_cast<unsigned>(total_written));
    return send_json(req, resp);
}

esp_err_t WebUiServer::handle_ota_check(httpd_req_t* req) {
    auto* self = static_cast<WebUiServer*>(req->user_ctx);
    self->touch_activity();

    std::string repo = kDefaultGitHubRepo;
    char qbuf[160]{};
    if (httpd_req_get_url_query_str(req, qbuf, sizeof(qbuf)) == ESP_OK) {
        char val[128]{};
        if (httpd_query_key_value(qbuf, "repo", val, sizeof(val)) == ESP_OK) {
            // Decode %2F -> /
            std::string dec;
            for (size_t i = 0; val[i]; ++i) {
                if (val[i] == '%' && val[i + 1] == '2' && (val[i + 2] == 'F' || val[i + 2] == 'f')) {
                    dec.push_back('/');
                    i += 2;
                } else {
                    dec.push_back(val[i]);
                }
            }
            if (!dec.empty()) repo = dec;
        }
    }

    const GitHubReleaseInfo info = check_github_release(repo);
    if (!info.ok) {
        return send_json(req, "{\"ok\":false,\"error\":" + json_escape(info.error) + "}");
    }
    std::string out = "{\"ok\":true,\"tag\":";
    out += json_escape(info.tag);
    out += ",\"asset_url\":";
    out += json_escape(info.asset_url);
    out += ",\"size\":";
    out += std::to_string(info.asset_size);
    out.push_back('}');
    return send_json(req, out);
}

esp_err_t WebUiServer::handle_ota_github(httpd_req_t* req) {
    auto* self = static_cast<WebUiServer*>(req->user_ctx);
    self->touch_activity();

    if (self->ota_in_progress()) {
        return send_json(req, R"({"ok":false,"error":"OTA already in progress"})", "409 Conflict");
    }
    std::string repo = kDefaultGitHubRepo;
    std::string url;
    if (req->content_len > 0 && req->content_len <= 1024) {
        std::string body(static_cast<size_t>(req->content_len), '\0');
        int rec = 0;
        while (rec < req->content_len) {
            const int r = httpd_req_recv(req, body.data() + rec, req->content_len - rec);
            if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
            if (r <= 0) break;
            rec += r;
        }
        JsonVal root;
        JsonParser p(body);
        if (p.parse(root) && root.type == JsonVal::Type::Object) {
            if (const auto* f = root.get("repo"); f && f->type == JsonVal::Type::String && !f->s.empty())
                repo = f->s;
            if (const auto* f = root.get("url"); f && f->type == JsonVal::Type::String)
                url = f->s;
        }
    }

    xSemaphoreTake(self->mutex_, portMAX_DELAY);
    self->gh_ota_repo_ = repo;
    self->gh_ota_url_ = url;
    self->state_.ota_failed = false;
    self->state_.ota_reboot = false;
    self->state_.ota_status = "Starting GitHub OTA...";
    self->state_.ota_progress.written.store(0, std::memory_order_release);
    self->state_.ota_progress.total.store(0, std::memory_order_release);
    self->state_.ota_progress.active.store(true, std::memory_order_release);
    xSemaphoreGive(self->mutex_);

    self->github_ota_requested_.store(true, std::memory_order_release);
    return send_json(req, R"({"ok":true})");
}

esp_err_t WebUiServer::handle_404(httpd_req_t* req, httpd_err_code_t) {
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    return httpd_resp_send(req, "Redirecting to NOTE4C Calendar...", HTTPD_RESP_USE_STRLEN);
}

} // namespace note4c::idf
