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
    if (nvs_get_blob(h, key, &cfg, &len) == ESP_OK) {
      out.push_back(cfg);
    }
  }
  nvs_close(h);
  return !out.empty();
}

static bool nvs_save_devices(const std::vector<StoredDeviceConfig> &devs) {
  nvs_handle_t h;
  if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return false;

  // Clear old entries
  uint8_t old_count = 0;
  nvs_get_u8(h, NVS_KEY_COUNT, &old_count);
  for (uint8_t i = 0; i < old_count; i++) {
    char key[8];
    snprintf(key, sizeof(key), "dev%d", i);
    nvs_erase_key(h, key);
  }

  uint8_t count = devs.size() < MAX_DEVICES ? devs.size() : MAX_DEVICES;
  nvs_set_u8(h, NVS_KEY_COUNT, count);

  for (uint8_t i = 0; i < count; i++) {
    char key[8];
    snprintf(key, sizeof(key), "dev%d", i);
    nvs_set_blob(h, key, &devs[i], sizeof(StoredDeviceConfig));
  }

  nvs_commit(h);
  nvs_close(h);
  ESP_LOGI(WEB_TAG, "Saved %d device configs to NVS", count);
  return true;
}

// ── Minimal JSON helpers (no external lib) ──────────────────
static void json_append_str(char *&p, const char *end, const char *key, const char *val, bool comma = true) {
  p += snprintf(p, end - p, "\"%s\":\"%s\"%s", key, val, comma ? "," : "");
}
static void json_append_int(char *&p, const char *end, const char *key, int val, bool comma = true) {
  p += snprintf(p, end - p, "\"%s\":%d%s", key, val, comma ? "," : "");
}
static void json_append_bool(char *&p, const char *end, const char *key, bool val, bool comma = true) {
  p += snprintf(p, end - p, "\"%s\":%s%s", key, val ? "true" : "false", comma ? "," : "");
}

// ── Simple JSON parser for config (no external lib) ─────────
// Finds value for "key" in a JSON object string. Returns pointer to value start.
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
  if (!v) return def;
  return atoi(v);
}

static bool json_get_bool(const char *json, const char *key, bool def = false) {
  const char *v = json_find_key(json, key);
  if (!v) return def;
  return (*v == 't' || *v == '1');
}

static void json_get_str(const char *json, const char *key, char *out, size_t max_len) {
  out[0] = 0;
  const char *v = json_find_key(json, key);
  if (!v || *v != '"') return;
  v++; // skip opening quote
  size_t i = 0;
  while (*v && *v != '"' && i < max_len - 1) {
    out[i++] = *v++;
  }
  out[i] = 0;
}

static int json_get_hex(const char *json, const char *key, int def = 0) {
  const char *v = json_find_key(json, key);
  if (!v) return def;
  if (*v == '"') v++; // skip optional quote
  return (int)strtol(v, nullptr, 16);
}

// ── HTML/CSS/JS embedded web page ───────────────────────────
static const char HTML_PAGE[] = R"rawliteral(<!DOCTYPE html>
<html><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>USB Gateway Config</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:-apple-system,sans-serif;background:#1a1a2e;color:#e0e0e0;padding:16px;max-width:800px;margin:0 auto}
h1{color:#4fc3f7;margin-bottom:8px;font-size:1.4em}
.sub{color:#888;font-size:.85em;margin-bottom:20px}
.card{background:#16213e;border-radius:8px;padding:16px;margin-bottom:12px;border:1px solid #2a3a5e}
.card.connected{border-left:3px solid #4caf50}
.card.disconnected{border-left:3px solid #666}
.row{display:flex;gap:10px;flex-wrap:wrap;margin-bottom:8px}
.row>*{flex:1;min-width:120px}
label{display:block;font-size:.75em;color:#888;margin-bottom:2px}
input,select{width:100%;padding:6px 8px;background:#0f1a30;border:1px solid #2a3a5e;border-radius:4px;color:#e0e0e0;font-size:.9em}
input:focus{border-color:#4fc3f7;outline:none}
.name-input{font-size:1.1em;font-weight:bold;border:none;background:transparent;color:#4fc3f7;padding:0;margin-bottom:4px}
.name-input:focus{border-bottom:1px solid #4fc3f7}
.btn{padding:8px 16px;border:none;border-radius:4px;cursor:pointer;font-size:.85em;font-weight:600}
.btn-primary{background:#4fc3f7;color:#1a1a2e}
.btn-danger{background:#ef5350;color:#fff}
.btn-secondary{background:#2a3a5e;color:#e0e0e0}
.btn:hover{opacity:.85}
.actions{display:flex;gap:8px;margin-top:8px;justify-content:flex-end}
.toolbar{display:flex;gap:8px;margin-bottom:16px;justify-content:space-between;align-items:center}
.status{font-size:.75em;padding:2px 8px;border-radius:10px;display:inline-block}
.status.on{background:#1b5e20;color:#81c784}
.status.off{background:#333;color:#888}
.toast{position:fixed;bottom:20px;left:50%;transform:translateX(-50%);background:#4caf50;color:#fff;padding:10px 24px;border-radius:4px;display:none;z-index:99;font-weight:600}
.toast.error{background:#ef5350}
.warn{background:#33291a;border:1px solid #ff9800;padding:10px;border-radius:4px;margin-bottom:12px;font-size:.85em;color:#ffb74d}
.chk{display:flex;align-items:center;gap:6px;min-width:auto;flex:0}
.chk input{width:auto}
</style>
</head><body>
<h1>USB TCP Gateway</h1>
<p class="sub">Configure USB device to TCP port mappings</p>
<div class="toolbar">
<button class="btn btn-primary" onclick="addDevice()">+ Add Device</button>
<div>
<button class="btn btn-secondary" onclick="loadConfig()">Reload</button>
<button class="btn btn-primary" onclick="saveConfig()">Save &amp; Apply</button>
</div>
</div>
<div class="warn">Changes require Save &amp; Apply, then the device reboots to apply new config.</div>
<div id="devices"></div>
<div id="toast" class="toast"></div>
<script>
let devices=[];
const BAUDS=[9600,19200,38400,57600,115200,230400,460800,921600];
function toast(msg,err){
  const t=document.getElementById('toast');
  t.textContent=msg;t.className='toast'+(err?' error':'');
  t.style.display='block';setTimeout(()=>t.style.display='none',3000);
}
function render(){
  const c=document.getElementById('devices');
  c.innerHTML='';
  devices.forEach((d,i)=>{
    const connected=d._connected||false;
    const cls=connected?'connected':'disconnected';
    const st=connected?'<span class="status on">Connected</span>':'<span class="status off">Offline</span>';
    c.innerHTML+=`<div class="card ${cls}">
      <input class="name-input" value="${esc(d.name)}" onchange="devices[${i}].name=this.value" placeholder="Device name">
      ${st}
      <div class="row">
        <div><label>VID (hex)</label><input value="${hex4(d.vid)}" onchange="devices[${i}].vid=parseInt(this.value,16)||0" maxlength="4" placeholder="0403"></div>
        <div><label>PID (hex)</label><input value="${hex4(d.pid)}" onchange="devices[${i}].pid=parseInt(this.value,16)||0" maxlength="4" placeholder="6001"></div>
        <div><label>TCP Port</label><input type="number" value="${d.port}" onchange="devices[${i}].port=parseInt(this.value)||8880" min="1024" max="65535"></div>
        <div><label>Baud Rate</label><select onchange="devices[${i}].baud_rate=parseInt(this.value)">
          ${BAUDS.map(b=>`<option value="${b}"${b===d.baud_rate?' selected':''}>${b}</option>`).join('')}
        </select></div>
      </div>
      <div class="row">
        <div><label>Interface</label><input type="number" value="${d.interface}" onchange="devices[${i}].interface=parseInt(this.value)||0" min="0" max="10"></div>
        <div class="chk"><input type="checkbox" id="ab${i}" ${d.autoboot?'checked':''} onchange="devices[${i}].autoboot=this.checked"><label for="ab${i}" style="color:#e0e0e0">Autoboot (no DTR/RTS toggle)</label></div>
      </div>
      <div class="actions"><button class="btn btn-danger" onclick="removeDevice(${i})">Delete</button></div>
    </div>`;
  });
}
function hex4(v){return(v||0).toString(16).toUpperCase().padStart(4,'0')}
function esc(s){return(s||'').replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]))}
function addDevice(){
  devices.push({name:'New Device',vid:0,pid:0,port:8880+devices.length,baud_rate:115200,interface:0,autoboot:false});
  render();
}
function removeDevice(i){devices.splice(i,1);render();}
function loadConfig(){
  fetch('/api/usb/config').then(r=>r.json()).then(d=>{devices=d;render();toast('Config loaded');}).catch(e=>toast('Load failed: '+e,true));
}
function saveConfig(){
  const body=JSON.stringify(devices.map(d=>({name:d.name,vid:d.vid,pid:d.pid,port:d.port,baud_rate:d.baud_rate,interface:d.interface,autoboot:d.autoboot})));
  fetch('/api/usb/config',{method:'POST',headers:{'Content-Type':'application/json'},body})
    .then(r=>{if(!r.ok)throw new Error(r.status);return r.json()})
    .then(d=>{toast(d.message||'Saved! Rebooting...');setTimeout(()=>location.reload(),8000);})
    .catch(e=>toast('Save failed: '+e,true));
}
loadConfig();
setInterval(()=>{
  fetch('/api/usb/status').then(r=>r.json()).then(st=>{
    st.forEach((s,i)=>{if(devices[i])devices[i]._connected=s.connected;});render();
  }).catch(()=>{});
},5000);
</script>
</body></html>)rawliteral";

// ── HTTP Handlers ───────────────────────────────────────────

// Forward declarations — these need access to the component
class UsbBridgeComponent;

struct WebContext {
  UsbBridgeComponent *component;
  httpd_handle_t server;
};

static WebContext web_ctx_;

// GET / — serve config page
static esp_err_t handle_root_(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html");
  httpd_resp_send(req, HTML_PAGE, sizeof(HTML_PAGE) - 1);
  return ESP_OK;
}

// GET /api/usb/config — return current device configs as JSON
static esp_err_t handle_get_config_(httpd_req_t *req);
// POST /api/usb/config — save device configs
static esp_err_t handle_post_config_(httpd_req_t *req);
// GET /api/usb/status — return connection status
static esp_err_t handle_get_status_(httpd_req_t *req);

static httpd_handle_t start_config_webserver_(int port) {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = port;
  config.max_uri_handlers = 8;
  config.stack_size = 8192;
  config.lru_purge_enable = true;
  config.max_resp_headers = 8;

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

  ESP_LOGI(WEB_TAG, "Config web UI available at http://<device-ip>:%d/", port);
  return server;
}

}  // namespace usb_bridge
}  // namespace esphome
