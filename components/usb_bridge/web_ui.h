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
static constexpr size_t MAX_DEVICES = 8;

struct StoredDeviceConfig {
  char name[32];
  uint16_t vid;
  uint16_t pid;
  uint16_t port;
  uint32_t baud_rate;
  uint8_t interface;
  uint8_t autoboot;
  uint8_t _pad[2];
};

static bool nvs_load_devices(std::vector<StoredDeviceConfig> &out) {
  nvs_handle_t h;
  if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return false;
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
  uint8_t old_count = 0;
  nvs_get_u8(h, NVS_KEY_COUNT, &old_count);
  for (uint8_t i = 0; i < old_count; i++) {
    char key[8]; snprintf(key, sizeof(key), "dev%d", i);
    nvs_erase_key(h, key);
  }
  uint8_t count = devs.size() < MAX_DEVICES ? devs.size() : MAX_DEVICES;
  nvs_set_u8(h, NVS_KEY_COUNT, count);
  for (uint8_t i = 0; i < count; i++) {
    char key[8]; snprintf(key, sizeof(key), "dev%d", i);
    nvs_set_blob(h, key, &devs[i], sizeof(StoredDeviceConfig));
  }
  nvs_commit(h);
  nvs_close(h);
  ESP_LOGI(WEB_TAG, "Saved %d device configs to NVS", count);
  return true;
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
<title>USB Gateway</title>
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
</style>
</head><body>
<h1>USB TCP Gateway</h1>
<p class="sub">Auto-detect USB devices, assign to TCP ports, save config</p>

<h2>Detected USB Devices</h2>
<div id="discovered"><div class="empty">Scanning USB bus...</div></div>

<h2>Configured Mappings</h2>
<div id="configured"><div class="empty">No devices configured yet. Use the + button on a detected device above.</div></div>

<div class="toolbar">
<button class="btn btn-secondary" onclick="loadAll()">Refresh</button>
<button class="btn btn-primary" onclick="saveConfig()">Save &amp; Reboot</button>
</div>

<div id="toast" class="toast"></div>
<script>
let configs=[];
let discovered=[];
const BAUDS=[9600,19200,38400,57600,115200,230400,460800,921600];

function toast(msg,err){
  const t=document.getElementById('toast');
  t.textContent=msg;t.className='toast'+(err?' error':'');
  t.style.display='block';setTimeout(()=>t.style.display='none',3000);
}
function hex4(v){return(v||0).toString(16).toUpperCase().padStart(4,'0')}
function esc(s){return(s||'').replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]))}

function chipName(vid){
  if(vid===0x0403)return'FTDI';if(vid===0x10C4)return'CP210X';
  if(vid===0x1A86)return'CH340';if(vid===0x2341)return'Arduino';
  return'USB Serial';
}

function renderDiscovered(){
  const c=document.getElementById('discovered');
  const devs=discovered.filter(d=>!d.hub);
  if(!devs.length){c.innerHTML='<div class="empty">No USB serial devices detected. Plug devices into the powered hub.</div>';return;}
  c.innerHTML='';
  devs.forEach(d=>{
    const assigned=d.assigned||configs.some(c=>c.vid===d.vid&&c.pid===d.pid);
    const st=assigned?'<span class="status on">Assigned</span>':'<span class="status avail">Available</span>';
    const name=d.product||chipName(d.vid);
    const mfr=d.manufacturer?d.manufacturer+' &middot; ':'';
    c.innerHTML+=`<div class="card discovered">
      <strong>${esc(name)}</strong> ${st}
      <div class="dev-info">${esc(mfr)}VID: ${hex4(d.vid)} &middot; PID: ${hex4(d.pid)} &middot; Intf: ${d.interfaces||1}${d.serial?' &middot; S/N: '+esc(d.serial):''}</div>
      ${assigned?'':'<button class="btn btn-add" onclick="addFromDevice('+d.vid+','+d.pid+',\''+esc(name)+'\')">+ Configure</button>'}
    </div>`;
  });
}

function renderConfigs(){
  const c=document.getElementById('configured');
  if(!configs.length){c.innerHTML='<div class="empty">No devices configured. Click + Configure on a detected device above.</div>';return;}
  c.innerHTML='';
  configs.forEach((d,i)=>{
    const conn=d._connected||false;
    const cls=conn?'connected':'disconnected';
    const st=conn?'<span class="status on">Connected</span>':'<span class="status off">Offline</span>';
    c.innerHTML+=`<div class="card ${cls}">
      <input class="name-input" value="${esc(d.name)}" onchange="configs[${i}].name=this.value" placeholder="Device name">
      ${st}
      <div class="dev-info">VID: ${hex4(d.vid)} &middot; PID: ${hex4(d.pid)} &middot; ${chipName(d.vid)}</div>
      <div class="row">
        <div><label>TCP Port</label><input type="number" value="${d.port}" onchange="configs[${i}].port=parseInt(this.value)||8880" min="1024" max="65535"></div>
        <div><label>Baud Rate</label><select onchange="configs[${i}].baud_rate=parseInt(this.value)">
          ${BAUDS.map(b=>`<option value="${b}"${b===d.baud_rate?' selected':''}>${b}</option>`).join('')}
        </select></div>
        <div><label>Interface</label><input type="number" value="${d.interface||0}" onchange="configs[${i}].interface=parseInt(this.value)||0" min="0" max="10"></div>
        <div class="chk"><input type="checkbox" id="ab${i}" ${d.autoboot?'checked':''} onchange="configs[${i}].autoboot=this.checked"><label for="ab${i}" style="color:#e0e0e0">Autoboot</label></div>
      </div>
      <div class="actions"><button class="btn btn-danger" onclick="removeConfig(${i})">Remove</button></div>
    </div>`;
  });
}

function addFromDevice(vid,pid,name){
  const next=8880+configs.length;
  configs.push({name:name||chipName(vid),vid,pid,port:next,baud_rate:115200,interface:0,autoboot:vid===0x10C4});
  renderConfigs();renderDiscovered();
  toast('Device added! Set TCP port and baud rate, then Save & Reboot.');
}

function removeConfig(i){configs.splice(i,1);renderConfigs();renderDiscovered();}

function loadAll(){
  Promise.all([
    fetch('/api/usb/config').then(r=>r.json()),
    fetch('/api/usb/status').then(r=>r.json())
  ]).then(([cfg,st])=>{
    configs=cfg;
    if(st.configured){
      st.configured.forEach((s,i)=>{if(configs[i])configs[i]._connected=s.connected;});
    }
    discovered=st.discovered||[];
    renderDiscovered();renderConfigs();
  }).catch(e=>toast('Load failed: '+e,true));
}

function saveConfig(){
  const body=JSON.stringify(configs.map(d=>({name:d.name,vid:d.vid,pid:d.pid,port:d.port,baud_rate:d.baud_rate,interface:d.interface||0,autoboot:!!d.autoboot})));
  fetch('/api/usb/config',{method:'POST',headers:{'Content-Type':'application/json'},body})
    .then(r=>{if(!r.ok)throw new Error(r.status);return r.json()})
    .then(d=>{toast(d.message||'Saved!');setTimeout(()=>location.reload(),10000);})
    .catch(e=>toast('Save failed: '+e,true));
}

loadAll();
setInterval(()=>{
  fetch('/api/usb/status').then(r=>r.json()).then(st=>{
    if(st.configured)st.configured.forEach((s,i)=>{if(configs[i])configs[i]._connected=s.connected;});
    discovered=st.discovered||[];
    renderDiscovered();renderConfigs();
  }).catch(()=>{});
},5000);
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
  httpd_resp_set_type(req, "text/html");
  httpd_resp_send(req, HTML_PAGE, sizeof(HTML_PAGE) - 1);
  return ESP_OK;
}

// Implemented after UsbBridgeComponent class definition in usb_bridge.h
static esp_err_t handle_get_config_(httpd_req_t *req);
static esp_err_t handle_post_config_(httpd_req_t *req);
static esp_err_t handle_get_status_(httpd_req_t *req);

static httpd_handle_t start_config_webserver_(int port) {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = port;
  config.max_uri_handlers = 8;
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

  httpd_register_uri_handler(server, &root);
  httpd_register_uri_handler(server, &get_cfg);
  httpd_register_uri_handler(server, &post_cfg);
  httpd_register_uri_handler(server, &get_st);

  ESP_LOGI(WEB_TAG, "Config UI at http://<ip>:%d/", port);
  return server;
}

}  // namespace usb_bridge
}  // namespace esphome
