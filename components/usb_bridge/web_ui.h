#pragma once

#include "esp_http_server.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esphome/core/log.h"

#include <cstring>
#include <cstdio>
#include <vector>

namespace esphome {
namespace usb_bridge {

static const char *const WEB_TAG = "usb_web";

// ── NVS Storage ─────────────────────────────────────────────
static constexpr const char *NVS_NAMESPACE = "usb_bridge";
static constexpr const char *NVS_KEY_COUNT = "dev_count";
static constexpr const char *NVS_KEY_VERSION = "cfg_ver";
static constexpr uint8_t NVS_CFG_VERSION = 3;  // v3: added per-device allowed_ips
static constexpr size_t MAX_DEVICES = 8;

struct StoredDeviceConfig {
  char name[32];
  uint16_t vid;
  uint16_t pid;
  uint16_t port;
  uint32_t baud_rate;
  uint8_t interface;
  uint8_t autoboot;
  char serial[64];
  char allowed_ips[128];  // per-device IP whitelist, comma-separated
};

static bool nvs_load_devices(std::vector<StoredDeviceConfig> &out) {
  nvs_handle_t h;
  if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return false;

  // Check config version
  uint8_t version = 0;
  nvs_get_u8(h, NVS_KEY_VERSION, &version);
  if (version != NVS_CFG_VERSION) {
    ESP_LOGW(WEB_TAG, "NVS config version mismatch (%d != %d), clearing", version, NVS_CFG_VERSION);
    nvs_close(h);
    // Erase old config
    nvs_handle_t hw;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &hw) == ESP_OK) {
      nvs_erase_all(hw);
      nvs_commit(hw);
      nvs_close(hw);
    }
    return false;
  }

  uint8_t count = 0;
  nvs_get_u8(h, NVS_KEY_COUNT, &count);
  out.clear();
  for (uint8_t i = 0; i < count && i < MAX_DEVICES; i++) {
    char key[8];
    snprintf(key, sizeof(key), "dev%d", i);
    StoredDeviceConfig cfg = {};
    size_t len = sizeof(cfg);
    if (nvs_get_blob(h, key, &cfg, &len) == ESP_OK) out.push_back(cfg);
  }
  nvs_close(h);
  return !out.empty();
}

static bool nvs_save_devices(const std::vector<StoredDeviceConfig> &devs) {
  nvs_handle_t h;
  if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return false;
  // Erase old entries
  uint8_t old_count = 0;
  nvs_get_u8(h, NVS_KEY_COUNT, &old_count);
  for (uint8_t i = 0; i < old_count; i++) {
    char key[8]; snprintf(key, sizeof(key), "dev%d", i);
    nvs_erase_key(h, key);
  }
  // Write new
  uint8_t count = devs.size() < MAX_DEVICES ? devs.size() : MAX_DEVICES;
  nvs_set_u8(h, NVS_KEY_COUNT, count);
  nvs_set_u8(h, NVS_KEY_VERSION, NVS_CFG_VERSION);
  for (uint8_t i = 0; i < count; i++) {
    char key[8]; snprintf(key, sizeof(key), "dev%d", i);
    nvs_set_blob(h, key, &devs[i], sizeof(StoredDeviceConfig));
  }
  nvs_commit(h);
  nvs_close(h);
  ESP_LOGI(WEB_TAG, "Saved %d device configs to NVS (v%d)", count, NVS_CFG_VERSION);
  return true;
}

// ── Bridge Settings (password, IP whitelist, MQTT) ──────────
struct BridgeSettings {
  char admin_password[32];
  char _reserved[256];     // was: global allowed_ips (now per-device)
  char mqtt_host[64];
  uint16_t mqtt_port;
  char mqtt_user[32];
  char mqtt_password[64];
  uint8_t mqtt_enabled;
  uint8_t mqtt_discovery;   // HA MQTT auto-discovery
  uint8_t _pad[2];
};

static bool nvs_load_settings(BridgeSettings &s) {
  memset(&s, 0, sizeof(s));
  s.mqtt_port = 1883;
  nvs_handle_t h;
  if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return false;
  size_t len = sizeof(s);
  bool ok = (nvs_get_blob(h, "settings", &s, &len) == ESP_OK);
  nvs_close(h);
  return ok;
}

static bool nvs_save_settings(const BridgeSettings &s) {
  nvs_handle_t h;
  if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return false;
  nvs_set_blob(h, "settings", &s, sizeof(s));
  nvs_commit(h);
  nvs_close(h);
  ESP_LOGI(WEB_TAG, "Settings saved to NVS");
  return true;
}

static bool is_ip_allowed(const char *ip, const char *allowed_list) {
  if (!allowed_list || !allowed_list[0]) return true;
  char list[256];
  strncpy(list, allowed_list, sizeof(list) - 1);
  list[sizeof(list) - 1] = 0;
  char *token = strtok(list, ", ");
  while (token) {
    if (strcmp(token, ip) == 0) return true;
    token = strtok(nullptr, ", ");
  }
  return false;
}

// Base64 decode (minimal, for Basic Auth)
static int base64_decode_(const char *in, char *out, size_t max_out) {
  static const int8_t T[128] = {
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,
    52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
    -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
    15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
    -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
    41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1};
  size_t len = strlen(in), o = 0;
  for (size_t i = 0; i < len && o < max_out - 1; i += 4) {
    uint32_t n = 0; int pad = 0;
    for (int j = 0; j < 4 && i + j < len; j++) {
      uint8_t c = (uint8_t)in[i + j];
      if (c == '=') { pad++; n <<= 6; }
      else if (c < 128 && T[c] >= 0) { n = (n << 6) | T[c]; }
      else { n <<= 6; }
    }
    if (o < max_out - 1) out[o++] = (n >> 16) & 0xFF;
    if (pad < 2 && o < max_out - 1) out[o++] = (n >> 8) & 0xFF;
    if (pad < 1 && o < max_out - 1) out[o++] = n & 0xFF;
  }
  out[o] = 0;
  return o;
}

static bool check_admin_auth_(httpd_req_t *req, const char *required_password) {
  if (!required_password || required_password[0] == '\0') return true;

  // Check HTTP Basic Auth: "Authorization: Basic base64(admin:password)"
  char auth_header[128] = {};
  if (httpd_req_get_hdr_value_str(req, "Authorization", auth_header, sizeof(auth_header)) == ESP_OK) {
    if (strncmp(auth_header, "Basic ", 6) == 0) {
      char decoded[96] = {};
      base64_decode_(auth_header + 6, decoded, sizeof(decoded));
      // Format: "admin:password" — skip "admin:" prefix
      char *colon = strchr(decoded, ':');
      if (colon && strcmp(colon + 1, required_password) == 0) return true;
    }
  }

  httpd_resp_set_status(req, "401 Unauthorized");
  httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"USB Gateway (user: admin)\"");
  httpd_resp_set_type(req, "text/plain");
  httpd_resp_sendstr(req, "Authentication required");
  return false;
}

// ── Minimal JSON helpers ────────────────────────────────────
static void json_append_str(char *&p, const char *end, const char *key, const char *val, bool comma = true) {
  p += snprintf(p, end - p, "\"%s\":\"%s\"%s", key, val, comma ? "," : "");
}
static void json_append_int(char *&p, const char *end, const char *key, int val, bool comma = true) {
  p += snprintf(p, end - p, "\"%s\":%d%s", key, val, comma ? "," : "");
}
static void json_append_bool(char *&p, const char *end, const char *key, bool val, bool comma = true) {
  p += snprintf(p, end - p, "\"%s\":%s%s", key, val ? "true" : "false", comma ? "," : "");
}

// ── Simple JSON parser ──────────────────────────────────────
static const char *json_find_key(const char *json, const char *key) {
  char search[48];
  snprintf(search, sizeof(search), "\"%s\"", key);
  const char *pos = strstr(json, search);
  if (!pos) return nullptr;
  pos += strlen(search);
  while (*pos == ' ' || *pos == ':') pos++;
  return pos;
}
static int json_get_int(const char *json, const char *key, int def = 0) {
  const char *v = json_find_key(json, key);
  return v ? atoi(v) : def;
}
static bool json_get_bool(const char *json, const char *key, bool def = false) {
  const char *v = json_find_key(json, key);
  return v ? (*v == 't' || *v == '1') : def;
}
static void json_get_str(const char *json, const char *key, char *out, size_t max_len) {
  out[0] = 0;
  const char *v = json_find_key(json, key);
  if (!v || *v != '"') return;
  v++;
  size_t i = 0;
  while (*v && *v != '"' && i < max_len - 1) out[i++] = *v++;
  out[i] = 0;
}

// ── HTML/CSS/JS embedded web page ───────────────────────────
static const char HTML_PAGE[] = R"rawliteral(<!DOCTYPE html>
<html><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title id="page-title-tag">USB Gateway</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:-apple-system,sans-serif;background:#1a1a2e;color:#e0e0e0;padding:16px;max-width:900px;margin:0 auto}
h1{color:#4fc3f7;margin-bottom:4px;font-size:1.4em}
h2{color:#90caf9;margin:20px 0 10px;font-size:1.1em;border-bottom:1px solid #2a3a5e;padding-bottom:4px}
.sub{color:#888;font-size:.85em;margin-bottom:16px}
.card{background:#16213e;border-radius:8px;padding:14px;margin-bottom:10px;border:1px solid #2a3a5e}
.card.connected{border-left:3px solid #4caf50}
.card.disconnected{border-left:3px solid #666}
.card.discovered{border-left:3px solid #ff9800;background:#1a2438}
.row{display:flex;gap:10px;flex-wrap:wrap;margin-bottom:8px}
.row>*{flex:1;min-width:100px}
label{display:block;font-size:.72em;color:#888;margin-bottom:2px;text-transform:uppercase}
input,select{width:100%;padding:5px 7px;background:#0f1a30;border:1px solid #2a3a5e;border-radius:4px;color:#e0e0e0;font-size:.9em}
input:focus{border-color:#4fc3f7;outline:none}
.name-input{font-size:1em;font-weight:bold;border:none;background:transparent;color:#4fc3f7;padding:0;margin-bottom:4px;width:60%}
.btn{padding:7px 14px;border:none;border-radius:4px;cursor:pointer;font-size:.82em;font-weight:600}
.btn-primary{background:#4fc3f7;color:#1a1a2e}
.btn-add{background:#ff9800;color:#1a1a2e}
.btn-danger{background:#ef5350;color:#fff}
.btn-secondary{background:#2a3a5e;color:#e0e0e0}
.btn:hover{opacity:.85}
.btn:disabled{opacity:.4;cursor:default}
.actions{display:flex;gap:6px;margin-top:6px;justify-content:flex-end}
.toolbar{display:flex;gap:8px;margin-bottom:12px;justify-content:flex-end}
.status{font-size:.72em;padding:2px 8px;border-radius:10px;display:inline-block;float:right}
.status.on{background:#1b5e20;color:#81c784}
.status.off{background:#333;color:#888}
.status.avail{background:#33291a;color:#ffb74d}
.toast{position:fixed;bottom:20px;left:50%;transform:translateX(-50%);background:#4caf50;color:#fff;padding:10px 24px;border-radius:4px;display:none;z-index:99;font-weight:600}
.toast.error{background:#ef5350}
.chk{display:flex;align-items:center;gap:6px;min-width:auto;flex:0}
.chk input{width:auto}
.dev-info{font-size:.82em;color:#aaa;margin-bottom:6px}
.empty{text-align:center;color:#666;padding:24px;font-size:.9em}
#logbox{background:#0a0e1a;border:1px solid #2a3a5e;border-radius:6px;padding:10px;font-family:monospace;font-size:.75em;color:#8f8;white-space:pre-wrap;word-break:break-all;max-height:300px;overflow-y:auto;margin-bottom:10px}
.chip-badge{font-size:.7em;padding:1px 6px;border-radius:8px;background:#2a3a5e;color:#90caf9;margin-left:6px}
</style>
</head><body>
<h1 id="page-title">USB TCP Gateway</h1>
<p class="sub">Auto-detect USB devices, assign to TCP ports, save config</p>

<h2>Detected USB Devices</h2>
<div id="discovered"><div class="empty">Scanning USB bus...</div></div>

<h2>Configured Mappings</h2>
<div id="configured"><div class="empty">No devices configured yet. Use the + button on a detected device above.</div></div>

<div class="toolbar">
<button class="btn btn-secondary" onclick="refreshStatus()">Refresh</button>
<button class="btn btn-primary" onclick="saveConfig()">Save &amp; Reboot</button>
</div>

<h2>Debug Log</h2>
<div id="logbox">Loading...</div>
<div class="toolbar">
<button class="btn btn-secondary" onclick="loadLog()">Refresh Log</button>
</div>

<h2>Settings</h2>
<div class="card" id="settings-panel">
<div class="row">
<div><label>Admin Password</label><div style="display:flex;gap:4px"><input type="password" id="s_password" placeholder="(none = open)" style="flex:1"><button class="btn btn-secondary" type="button" onclick="const p=document.getElementById('s_password');p.type=p.type==='password'?'text':'password';this.textContent=p.type==='password'?'Show':'Hide'" style="padding:4px 10px;font-size:.8em">Show</button><button class="btn btn-danger" type="button" id="s_pw_clear" style="padding:4px 10px;font-size:.8em;display:none" onclick="clearAdminPassword()">Clear</button></div></div>
</div>
<div class="row">
<div><label>MQTT Host</label><input type="text" id="s_mqtt_host" placeholder="192.168.1.x"></div>
<div><label>MQTT Port</label><input type="number" id="s_mqtt_port" value="1883" min="1" max="65535"></div>
</div>
<div class="row">
<div><label>MQTT User</label><input type="text" id="s_mqtt_user" placeholder="(optional)"></div>
<div><label>MQTT Password</label><div style="display:flex;gap:4px"><input type="password" id="s_mqtt_pass" placeholder="(optional)" style="flex:1"><button class="btn btn-secondary" type="button" onclick="const p=document.getElementById('s_mqtt_pass');p.type=p.type==='password'?'text':'password';this.textContent=p.type==='password'?'Show':'Hide'" style="padding:4px 10px;font-size:.8em">Show</button></div></div>
<div class="chk"><input type="checkbox" id="s_mqtt_en"><label for="s_mqtt_en" style="color:#e0e0e0">MQTT enabled</label></div>
<div class="chk"><input type="checkbox" id="s_mqtt_disc"><label for="s_mqtt_disc" style="color:#e0e0e0">HA Auto Discovery</label></div>
</div>
<div class="toolbar">
<button class="btn btn-primary" onclick="saveSettings()">Save Settings</button>
</div>
</div>

<div id="toast" class="toast"></div>
<script>
let configs=[];
let savedConfigs=[];
let discovered=[];
let dirty=false;
const BAUDS=[9600,19200,38400,57600,115200,230400,460800,921600];

function toast(msg,err){
  const t=document.getElementById('toast');
  t.textContent=msg;t.className='toast'+(err?' error':'');
  t.style.display='block';setTimeout(()=>t.style.display='none',3000);
}
function hex4(v){return(v||0).toString(16).toUpperCase().padStart(4,'0')}
function esc(s){return(s||'').replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]))}

function isAssigned(d){
  if(d.assigned) return true;
  return configs.some(c=>c.vid===d.vid&&c.pid===d.pid&&
    (!c.serial||!d.serial||c.serial===d.serial));
}

function renderDiscovered(){
  const c=document.getElementById('discovered');
  const devs=discovered.filter(d=>!d.hub);
  if(!devs.length){c.innerHTML='<div class="empty">No USB serial devices detected. Plug devices into the powered hub.</div>';return;}
  c.innerHTML='';
  devs.forEach(d=>{
    const assigned=isAssigned(d);
    const st=assigned?'<span class="status on">Assigned</span>':'<span class="status avail">Available</span>';
    const name=d.product||('USB Device '+hex4(d.vid)+':'+hex4(d.pid));
    const mfr=d.manufacturer||'';
    const sn=d.serial?' &middot; S/N: '+esc(d.serial):'';
    c.innerHTML+=`<div class="card discovered">
      <strong>${esc(name)}</strong> ${mfr?'<span class="chip-badge">'+esc(mfr)+'</span>':''} ${st}
      <div class="dev-info">VID: ${hex4(d.vid)} &middot; PID: ${hex4(d.pid)} &middot; Addr: ${d.addr} &middot; Intf: ${d.interfaces||1}${sn}</div>
      ${assigned?'':'<button class="btn btn-add" onclick=\'addFromDevice('+d.vid+','+d.pid+','+JSON.stringify(name)+','+JSON.stringify(d.serial||"")+','+d.interfaces+')\'>+ Configure</button>'}
    </div>`;
  });
}

function renderConfigs(){
  const c=document.getElementById('configured');
  if(!configs.length){c.innerHTML='<div class="empty">No devices configured. Click + Configure on a detected device above.</div>';return;}
  c.innerHTML='';
  configs.forEach((d,i)=>{
    const conn=d._connected||false;
    const chip=d._chip||'';
    const cls=conn?'connected':'disconnected';
    const st=conn?'<span class="status on">Connected</span>':'<span class="status off">Offline</span>';
    c.innerHTML+=`<div class="card ${cls}">
      <input class="name-input" value="${esc(d.name)}" onchange="configs[${i}].name=this.value;dirty=true" placeholder="Device name">
      ${chip?'<span class="chip-badge">'+esc(chip)+'</span>':''} ${st}
      <div class="dev-info">VID: ${hex4(d.vid)} &middot; PID: ${hex4(d.pid)}${d.serial?' &middot; S/N: '+esc(d.serial):''}</div>
      <div class="row">
        <div><label>TCP Port</label><input type="number" value="${d.port}" onchange="configs[${i}].port=parseInt(this.value)||8880;dirty=true" min="1024" max="65535"></div>
        <div><label>Baud Rate</label><select onchange="configs[${i}].baud_rate=parseInt(this.value);dirty=true">
          ${BAUDS.map(b=>`<option value="${b}"${b===d.baud_rate?' selected':''}>${b}</option>`).join('')}
        </select></div>
        <div><label>Interface</label><input type="number" value="${d.interface||0}" onchange="configs[${i}].interface=parseInt(this.value)||0;dirty=true" min="0" max="10"></div>
        <div class="chk"><input type="checkbox" id="ab${i}" ${d.autoboot?'checked':''} onchange="configs[${i}].autoboot=this.checked;dirty=true"><label for="ab${i}" style="color:#e0e0e0">Autoboot</label></div>
      </div>
      <div class="row">
        <div style="flex:1"><label>Allowed IPs</label><input type="text" value="${esc(d.allowed_ips||'')}" onchange="configs[${i}].allowed_ips=this.value;dirty=true" placeholder="empty = all allowed" title="Comma-separated IPs allowed to connect to this TCP port"></div>
      </div>
      <div class="actions"><button class="btn btn-danger" onclick="removeConfig(${i})">Remove</button></div>
    </div>`;
  });
}

function addFromDevice(vid,pid,name,serial,intfCount){
  if(configs.some(c=>c.vid===vid&&c.pid===pid&&(!serial||c.serial===serial))){
    toast('Device already configured!',true);return;
  }
  const next=8880+configs.length;
  // For CDC-ACM devices with multiple interfaces, default to interface 0
  // (interface 0 = CDC control, interface 1 = CDC data — we need data interface for bulk endpoints)
  let defIntf = 0;
  if(intfCount>1) defIntf=1;  // CDC data interface is usually 1
  configs.push({name:name||('Device '+hex4(vid)+':'+hex4(pid)),vid,pid,serial:serial||'',port:next,baud_rate:115200,interface:defIntf,autoboot:false,allowed_ips:''});
  dirty=true;
  renderConfigs();renderDiscovered();
  toast('Device added! Set TCP port and baud rate, then Save & Reboot.');
}

function removeConfig(i){configs.splice(i,1);dirty=true;renderConfigs();renderDiscovered();}

function loadAll(){
  Promise.all([
    fetch('/api/usb/config').then(r=>r.json()),
    fetch('/api/usb/status').then(r=>r.json())
  ]).then(([cfg,st])=>{
    savedConfigs=JSON.parse(JSON.stringify(cfg));
    configs=cfg;
    dirty=false;
    if(st.configured){
      st.configured.forEach((s,i)=>{
        if(configs[i]){configs[i]._connected=s.connected;configs[i]._chip=s.chip||'';}
      });
    }
    discovered=st.discovered||[];
    if(st.friendly_name){
      document.getElementById('page-title').textContent=st.friendly_name;
      document.getElementById('page-title-tag').textContent=st.friendly_name;
    }
    renderDiscovered();renderConfigs();
  }).catch(e=>toast('Load failed: '+e,true));
}

function refreshStatus(){
  fetch('/api/usb/status').then(r=>r.json()).then(st=>{
    if(st.configured){
      st.configured.forEach((s,i)=>{
        if(configs[i]){configs[i]._connected=s.connected;configs[i]._chip=s.chip||'';}
      });
    }
    discovered=st.discovered||[];
    renderDiscovered();renderConfigs();
    toast('Refreshed');
  }).catch(e=>toast('Refresh failed: '+e,true));
}

function getPassword(){
  const pw=document.getElementById('s_password').value;
  if(pw) return pw;
  if(!window._hasAdminPw) return '';
  const entered=prompt('Admin password required:');
  return entered||'';
}
function authHeaders(extra){
  const h=Object.assign({},extra||{});
  const pw=getPassword();
  if(pw) h['Authorization']='Basic '+btoa('admin:'+pw);
  return h;
}

function saveConfig(){
  const body=JSON.stringify(configs.map(d=>({
    name:d.name,vid:d.vid,pid:d.pid,serial:d.serial||'',
    port:d.port,baud_rate:d.baud_rate,interface:d.interface||0,autoboot:!!d.autoboot,
    allowed_ips:d.allowed_ips||''
  })));
  fetch('/api/usb/config',{method:'POST',headers:authHeaders({'Content-Type':'application/json'}),body})
    .then(r=>{if(r.status===401)throw new Error('Wrong password');if(!r.ok)throw new Error(r.status);return r.json()})
    .then(d=>{
      dirty=false;
      toast(d.message||'Saved!');
      setTimeout(()=>location.reload(),10000);
    })
    .catch(e=>toast('Save failed: '+e,true));
}

function loadSettings(){
  fetch('/api/usb/settings').then(r=>r.json()).then(s=>{
    document.getElementById('s_password').value='';
    document.getElementById('s_password').placeholder=s.has_password?'(password set — enter to change)':'(none = open)';
    document.getElementById('s_mqtt_host').value=s.mqtt_host||'';
    document.getElementById('s_mqtt_port').value=s.mqtt_port||1883;
    document.getElementById('s_mqtt_user').value=s.mqtt_user||'';
    document.getElementById('s_mqtt_pass').value='';
    document.getElementById('s_mqtt_pass').placeholder=s.has_mqtt_pass?'(set — enter to change)':'(optional)';
    document.getElementById('s_mqtt_en').checked=!!s.mqtt_enabled;
    document.getElementById('s_mqtt_disc').checked=!!s.mqtt_discovery;
    window._hasAdminPw=!!s.has_password;
    document.getElementById('s_pw_clear').style.display=s.has_password?'inline-block':'none';
  }).catch(()=>{});
}

function saveSettings(){
  const body=JSON.stringify({
    password:document.getElementById('s_password').value,
    mqtt_host:document.getElementById('s_mqtt_host').value,
    mqtt_port:parseInt(document.getElementById('s_mqtt_port').value)||1883,
    mqtt_user:document.getElementById('s_mqtt_user').value,
    mqtt_pass:document.getElementById('s_mqtt_pass').value,
    mqtt_enabled:document.getElementById('s_mqtt_en').checked,
    mqtt_discovery:document.getElementById('s_mqtt_disc').checked
  });
  fetch('/api/usb/settings',{method:'POST',headers:authHeaders({'Content-Type':'application/json'}),body})
    .then(r=>{if(r.status===401)throw new Error('Wrong password');if(!r.ok)throw new Error(r.status);return r.json()})
    .then(d=>toast(d.message||'Settings saved!'))
    .catch(e=>toast('Settings save failed: '+e,true));
}

function clearAdminPassword(){
  if(!confirm('Remove admin password? Settings will be unprotected.')) return;
  fetch('/api/usb/settings/clear-password',{method:'POST',headers:authHeaders()})
    .then(r=>{if(r.status===401)throw new Error('Wrong password');if(!r.ok)throw new Error(r.status);return r.json()})
    .then(d=>{toast(d.message||'Password cleared');loadSettings();})
    .catch(e=>toast('Failed: '+e,true));
}

function loadLog(){
  fetch('/api/usb/log').then(r=>r.text()).then(t=>{
    const el=document.getElementById('logbox');
    el.textContent=t||'(empty)';
    el.scrollTop=el.scrollHeight;
  }).catch(e=>{document.getElementById('logbox').textContent='Failed: '+e});
}

// Initial load
loadAll();
loadLog();
loadSettings();

// Auto-refresh status (don't overwrite local unsaved config changes)
setInterval(()=>{
  fetch('/api/usb/status').then(r=>r.json()).then(st=>{
    if(st.configured){
      st.configured.forEach((s,i)=>{
        if(configs[i]){configs[i]._connected=s.connected;configs[i]._chip=s.chip||'';}
      });
    }
    discovered=st.discovered||[];
    renderDiscovered();renderConfigs();
  }).catch(()=>{});
},5000);

// Auto-refresh log
setInterval(loadLog,10000);
</script>
</body></html>)rawliteral";

// ── HTTP Handler forward declarations ───────────────────────
class UsbBridgeComponent;

struct WebContext {
  UsbBridgeComponent *component;
  httpd_handle_t server;
};

static WebContext web_ctx_;

static esp_err_t handle_root_(httpd_req_t *req) {
  BridgeSettings s;
  nvs_load_settings(s);
  if (!check_admin_auth_(req, s.admin_password)) return ESP_OK;
  httpd_resp_set_type(req, "text/html");
  httpd_resp_send(req, HTML_PAGE, sizeof(HTML_PAGE) - 1);
  return ESP_OK;
}

// Implemented after UsbBridgeComponent class definition in usb_bridge.h
static esp_err_t handle_get_config_(httpd_req_t *req);
static esp_err_t handle_post_config_(httpd_req_t *req);
static esp_err_t handle_get_status_(httpd_req_t *req);
static esp_err_t handle_get_log_(httpd_req_t *req);

// Settings handlers (self-contained, no component access needed)
static esp_err_t handle_get_settings_(httpd_req_t *req) {
  BridgeSettings s;
  nvs_load_settings(s);
  char buf[512]; char *p = buf; const char *end = buf + sizeof(buf) - 2;
  *p++ = '{';
  json_append_bool(p, end, "has_password", s.admin_password[0] != '\0');
  json_append_str(p, end, "mqtt_host", s.mqtt_host);
  json_append_int(p, end, "mqtt_port", s.mqtt_port);
  json_append_str(p, end, "mqtt_user", s.mqtt_user);
  json_append_bool(p, end, "has_mqtt_pass", s.mqtt_password[0] != '\0');
  json_append_bool(p, end, "mqtt_enabled", s.mqtt_enabled);
  json_append_bool(p, end, "mqtt_discovery", s.mqtt_discovery, false);
  *p++ = '}'; *p = 0;
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, buf, p - buf);
  return ESP_OK;
}

static esp_err_t handle_post_settings_(httpd_req_t *req) {
  // Check auth with current password
  BridgeSettings cur;
  nvs_load_settings(cur);
  if (!check_admin_auth_(req, cur.admin_password)) return ESP_OK;

  int total_len = req->content_len;
  if (total_len > 1024) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Too large"); return ESP_FAIL; }
  char *body = (char *)malloc(total_len + 1);
  if (!body) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM"); return ESP_FAIL; }
  int received = 0;
  while (received < total_len) {
    int ret = httpd_req_recv(req, body + received, total_len - received);
    if (ret <= 0) { free(body); return ESP_FAIL; }
    received += ret;
  }
  body[total_len] = 0;

  BridgeSettings s = cur;  // start from current settings
  char new_pw[32] = {};
  char new_mqtt_pw[64] = {};
  json_get_str(body, "password", new_pw, sizeof(new_pw));
  if (new_pw[0]) strncpy(s.admin_password, new_pw, sizeof(s.admin_password) - 1);
  json_get_str(body, "mqtt_host", s.mqtt_host, sizeof(s.mqtt_host));
  s.mqtt_port = json_get_int(body, "mqtt_port", 1883);
  json_get_str(body, "mqtt_user", s.mqtt_user, sizeof(s.mqtt_user));
  json_get_str(body, "mqtt_pass", new_mqtt_pw, sizeof(new_mqtt_pw));
  if (new_mqtt_pw[0]) strncpy(s.mqtt_password, new_mqtt_pw, sizeof(s.mqtt_password) - 1);
  s.mqtt_enabled = json_get_bool(body, "mqtt_enabled", false) ? 1 : 0;
  s.mqtt_discovery = json_get_bool(body, "mqtt_discovery", false) ? 1 : 0;
  free(body);

  nvs_save_settings(s);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr(req, "{\"message\":\"Settings saved! Reboot to apply MQTT changes.\",\"ok\":true}");
  return ESP_OK;
}

static esp_err_t handle_clear_password_(httpd_req_t *req) {
  BridgeSettings cur;
  nvs_load_settings(cur);
  if (!check_admin_auth_(req, cur.admin_password)) return ESP_OK;
  cur.admin_password[0] = '\0';
  nvs_save_settings(cur);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr(req, "{\"message\":\"Admin password removed.\",\"ok\":true}");
  return ESP_OK;
}

static httpd_handle_t start_config_webserver_(int port) {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = port;
  config.max_uri_handlers = 10;
  config.stack_size = 8192;
  config.lru_purge_enable = true;

  httpd_handle_t server = nullptr;
  if (httpd_start(&server, &config) != ESP_OK) {
    ESP_LOGE(WEB_TAG, "Failed to start config web server on port %d", port);
    return nullptr;
  }

  httpd_uri_t root = {.uri = "/", .method = HTTP_GET, .handler = handle_root_, .user_ctx = nullptr};
  httpd_uri_t get_cfg = {.uri = "/api/usb/config", .method = HTTP_GET, .handler = handle_get_config_, .user_ctx = nullptr};
  httpd_uri_t post_cfg = {.uri = "/api/usb/config", .method = HTTP_POST, .handler = handle_post_config_, .user_ctx = nullptr};
  httpd_uri_t get_st = {.uri = "/api/usb/status", .method = HTTP_GET, .handler = handle_get_status_, .user_ctx = nullptr};
  httpd_uri_t get_log = {.uri = "/api/usb/log", .method = HTTP_GET, .handler = handle_get_log_, .user_ctx = nullptr};
  httpd_uri_t get_set = {.uri = "/api/usb/settings", .method = HTTP_GET, .handler = handle_get_settings_, .user_ctx = nullptr};
  httpd_uri_t post_set = {.uri = "/api/usb/settings", .method = HTTP_POST, .handler = handle_post_settings_, .user_ctx = nullptr};
  httpd_uri_t clr_pw = {.uri = "/api/usb/settings/clear-password", .method = HTTP_POST, .handler = handle_clear_password_, .user_ctx = nullptr};

  httpd_register_uri_handler(server, &root);
  httpd_register_uri_handler(server, &get_cfg);
  httpd_register_uri_handler(server, &post_cfg);
  httpd_register_uri_handler(server, &get_st);
  httpd_register_uri_handler(server, &get_log);
  httpd_register_uri_handler(server, &get_set);
  httpd_register_uri_handler(server, &post_set);
  httpd_register_uri_handler(server, &clr_pw);

  ESP_LOGI(WEB_TAG, "Config UI at http://<ip>:%d/", port);
  return server;
}

}  // namespace usb_bridge
}  // namespace esphome
