# ESP-HA-USB-Gateway Development Notes

## Project Goal
Generic ESP32-S3 USB-to-TCP bridge for Home Assistant. Replaces USB passthrough
to Proxmox VMs, enabling VM live migration. Auto-detects USB devices, configurable
via web UI — no YAML device config needed.

## Hardware Setup

### ESP32-S3-DevKitC-1
- **IP**: 192.168.1.108
- **ESPHome name**: usb-bridge
- **Two USB-C ports**: UART (programming/power) and OTG (USB host)
- **CRITICAL: OTG port does NOT provide 5V VBUS** — a **powered USB hub is
  REQUIRED**. Do NOT suggest direct connection or hardware modification.
- **OTG port**: GPIO19 (D-) / GPIO20 (D+), used for USB host mode

### Powered USB Hub
- **VID=1A40 PID=0201** — generic USB 2.0 hub chip
- Required because ESP32-S3 OTG provides no VBUS
- Hub appears as first enumerated device, downstream devices follow

### Verified USB Devices
| Device | VID:PID | Chip | Baud | Notes |
|--------|---------|------|------|-------|
| SkyConnect v1.0 | 10C4:EA60 | CP210X | 115200 | autoboot=true (no DTR toggle!), S/N: 581c7557bf9ced11baa37ffaa7669f5d |
| Sonoff Zigbee 3.0 V2 | 10C4:EA60 | CP210X | 115200 | **SAME VID:PID as SkyConnect!** S/N: 847a1592b049ef11a799d58cff00cc63 |
| EnOcean USB 300 | 0403:6001 | FTDI FT232 | 57600 | product string: "USB <-> Serial" |
| RFLink (Arduino Mega) | 2341:0042 | ATmega16U2 | 57600 | CDC-ACM |

**CRITICAL**: SkyConnect and Sonoff both use CP210X with identical VID:PID (10C4:EA60).
Serial number matching is REQUIRED to distinguish them.

## Architecture

### ESPHome Component
```
components/usb_bridge/
  __init__.py    — minimal config schema (just usb_bridge:), sdkconfig options
  usb_bridge.h   — USB host, device matching, TCP servers, HTTP handlers
  web_ui.h       — embedded HTML/CSS/JS web UI, NVS storage helpers
esphome/
  usb-bridge.yaml — device config (no device list needed in YAML)
```

### How It Works
1. ESP32 boots, installs USB host (IDF handles PHY init)
2. Built-in ESP-IDF hub driver enumerates hub + downstream devices
3. Each detected device appears in `discovered_` list
4. If a NVS-saved config matches (VID/PID/Serial), device is auto-assigned to TCP port
5. Unconfigured devices show as "Available" in web UI at port 80
6. User clicks "+ Configure", sets TCP port/baud/etc., clicks "Save & Reboot"
7. Config is saved to NVS flash, device reboots, devices auto-connect

### FreeRTOS Tasks
| Task | Stack | Core | Priority | Purpose |
|------|-------|------|----------|---------|
| usb_lib | 8192 | 0 | 10 | USB library event daemon |
| usb_mon | 8192 | 1 | 5 | Client event handler (device connect/disconnect) |
| tcp_srv | 8192 | 1 | 5 | TCP server per configured device |
| usb_rx | 4096 | 1 | 6 | Bulk IN reader per connected device |

### Web UI (port 80)
- **Detected Devices**: auto-populated from USB bus scan (all devices, no filter)
- **Configured Mappings**: saved device-to-TCP-port assignments
- REST API: `GET/POST /api/usb/config`, `GET /api/usb/status`, `GET /api/usb/log`
- Config stored in NVS flash (survives reboots, no YAML needed)
- Save & Reboot: saves config, cleanly shuts down USB, then reboots

### Chip Support
| Type | Detection | Init Protocol |
|------|-----------|---------------|
| FTDI | VID=0403 | Vendor control requests (reset, baud, data, flow) |
| CP210X | VID=10C4 | Vendor requests (IFC_ENABLE, SET_MHS, SET_BAUDRATE) |
| CDC-ACM | Interface class 0x02 | Standard SET_LINE_CODING |
| Generic | Fallback | Bulk endpoints only, no init |

## sdkconfig Options
```
CONFIG_USB_OTG_SUPPORTED=y
CONFIG_USB_HOST_CONTROL_TRANSFER_MAX_SIZE=1024
CONFIG_USB_HOST_HUBS_SUPPORTED=y
CONFIG_USB_HOST_HUB_MULTI_LEVEL=y
CONFIG_USB_HOST_ENABLE_ENUM_FILTER_CALLBACK=y  # Required for enum_filter_cb struct field
CONFIG_SPIRAM=n                    # PSRAM breaks USB host (IDF #9519)
CONFIG_ESP32S3_SPIRAM_SUPPORT=n
CONFIG_HTTPD_MAX_REQ_HDR_LEN=1024  # For config web UI
```

## ESP-IDF Version
- ESPHome 2026.3.1 uses **ESP-IDF 5.5.2** (`version: recommended`)
- Hub support since ESP-IDF 5.2
- ESP32-S3: only **8 USB host channels** (hub + 2 devices can exhaust them)

## USB Device Detection After Reboot (Solved 2026-04-01)

### Problem
After ESP reboot (Save&Reboot or power cycle), USB devices are NOT detected.
Only physical USB cable disconnect/reconnect works. Hub never re-enumerates.

### Root Causes Found

1. **GPIO SE0 does NOT work on ESP32-S3 internal PHY** — GPIO19/20 are isolated
   by the USB PHY hardware. Writing LOW to these pins has no effect on the actual
   USB D+/D- lines. All GPIO-based reset approaches are a dead end.

2. **`enum_filter_cb = nullptr` with `CONFIG_USB_HOST_ENABLE_ENUM_FILTER_CALLBACK=y`**
   causes ESP-IDF to silently skip device enumeration. Zero devices detected, even
   on physical reconnect. Must use a real callback function (can return `true` for all).

3. **`USB_SERIAL_JTAG` logger (ESPHome 2026.3.x default on ESP32-S3)** blocks
   GPIO19/20 needed for USB OTG host mode. Must set `hardware_uart: UART0` in YAML.

4. **`usb_host_install()` does not reliably detect already-connected devices** when
   the hub was never properly disconnected from the host.

### Solution (Build 2026-04-01-f)

**Warm Reboot (Save & Reboot):**
Before `esp_restart()`, cleanly shut down the USB stack:
```cpp
// Close all devices → deregister client → usb_host_uninstall()
// This turns OFF the PHY → hub sees real electrical disconnect
// 500ms delay → esp_restart() → usb_host_install() → hub sees connect
```

**Cold Boot (Power Cycle):**
ESP and hub power up simultaneously. Hub may not be ready when
`usb_host_install()` runs. Retry mechanism in `loop()`:
- After 10 seconds with 0 devices → uninstall + reinstall USB host stack
- Second attempt catches the now-ready hub

**Enum Filter:**
```cpp
// Real callback function that allows ALL devices (returns true)
// NOT nullptr — nullptr breaks enumeration with ENABLE_ENUM_FILTER_CALLBACK
static bool enum_filter_allow_all_(const usb_device_desc_t *dev_desc, uint8_t *bConfigurationValue) {
    return true;
}
```

### Version History (Reboot Fix)
| Build | Approach | Result |
|-------|----------|--------|
| 2026-03-31-h (29fd2bf) | GPIO SE0 2×100ms + real enum_filter_cb_ | Worked sometimes (warm reboot OK, cold boot intermittent) |
| 2026-04-01-a | Removed enum filter (nullptr) | **BROKE ALL** — 0 devices, even on reconnect |
| 2026-04-01-b | No GPIO, no filter, UART0 fix | Stable but 0 devices |
| 2026-04-01-c | Restored enum_filter_allow_all_ + SE0 + UART0 | Works after cable reconnect only |
| 2026-04-01-d | 3s cold-boot wait before SE0 | First boot OK, subsequent reboots fail |
| 2026-04-01-e | SE0 immediately before usb_host_install() | Still no devices after reboot |
| **2026-04-01-f** | **Clean USB shutdown before reboot + cold boot retry** | **All 3 test cases pass ✅** |

### Key Learnings
- ESP32-S3 internal USB PHY isolates GPIO19/20 — cannot manipulate USB bus via GPIO
- `CONFIG_USB_HOST_ENABLE_ENUM_FILTER_CALLBACK` must be enabled AND callback must be non-null
- `USB_SERIAL_JTAG` and USB OTG share GPIO19/20 — cannot use both
- USB hub must see proper electrical disconnect to re-enumerate devices
- `usb_host_uninstall()` properly shuts down PHY (removes pull-downs)

## Known Limitations
- **8 HCD channels**: ESP32-S3 limit. Hub + 2 serial devices is the practical max.
- **No hot-add of configs**: changing config requires reboot (Save & Reboot).
- **Single TCP client per port**: one client at a time per device.

## Tested Device Combinations (2026-04-01)
| Combo | Sticks | Result |
|-------|--------|--------|
| SkyConnect + Sonoff Zigbee 3.0 | 2x CP210X | ✅ Both detected and functional |
| SkyConnect + EnOcean USB 300 | CP210X + FTDI | ❌ Only EnOcean active, SkyConnect dropped (HCD channel limit) |
| Sonoff + EnOcean USB 300 | CP210X + FTDI | ❌ Same issue — 3rd device exceeds channel limit |

**Conclusion**: With USB hub, max 2 serial devices simultaneously.
Without hub (USB-C OTG splitter) may be better, as hub itself uses channels.

## Integration Tests (2026-04-01)
| Integration | Stick | Transport | Status |
|-------------|-------|-----------|--------|
| ZHA | SkyConnect | Direct USB on HA host | ✅ Running |
| Zigbee2MQTT 2.9.1 | Sonoff Zigbee 3.0 V2 | TCP via ESP Bridge (port 8880) | ✅ Running |
| Z2M + SONOFF SNZB-06P | Sonoff | TCP | ✅ Paired and functional |
| Z2M + EnOcean PTM 215ZE | Sonoff | TCP | ✅ Green Power device paired |
| EnOcean MQTT UI | EnOcean USB 300 | TCP via ESP Bridge | ✅ (when alone or with 1 other stick) |

## Planned Improvements
- [ ] USB-C OTG splitter instead of large hub (saves HCD channels)
- [ ] Web UI: documentation, images, menu structure
- [ ] Web UI: password protection (prevent reconfiguration from network)
- [ ] Test if more than 2 devices work simultaneously without hub

## Solved Issues Reference
| Issue | Fix |
|-------|-----|
| Root port reset failed | Client registration order — register in setup() before starting tasks |
| Control transfer deadlock | Don't do transfers inside event callback; queue addr, process in monitor task |
| WDT crash | Single client registration in setup() |
| Missing endpoints | Manual descriptor walking instead of ESP-IDF parser |
| lwip socket() conflict | Use lwip_socket() etc. directly |
| ESPHome git cache | Clean Build Files required after changes |
| PSRAM + USB conflict | Disable PSRAM via sdkconfig |
| Hub CP2102 exhausts HCD channels | Enum filter rejects 3rd+ CP210x (hub-internal bridge) |
| ESP offline after update | `CONFIG_USB_HOST_ENABLE_ENUM_FILTER_CALLBACK` removed but `enum_filter_cb` still referenced |
| Serial matching too strict | Restored VID/PID fallback when device reports no serial |
| enum_filter_cb=nullptr kills enumeration | With ENABLE_ENUM_FILTER_CALLBACK=y, nullptr causes IDF to skip all enumeration. Use real function returning true |
| USB_SERIAL_JTAG blocks USB OTG | ESPHome 2026.3.x defaults to USB_SERIAL_JTAG on ESP32-S3, shares GPIO19/20. Fix: `hardware_uart: UART0` |
| Boot loop (RTC_SW_CPU_RST) | `#include "esphome/core/application.h"` and `hal.h` cause static init crash. Use vTaskDelay() instead |
| GPIO SE0 ineffective on ESP32-S3 | Internal PHY isolates GPIO19/20 from USB bus. Removed GPIO approach entirely |
| Devices not detected after reboot | USB stack not cleanly shut down before esp_restart(). Fix: close devices + deregister + usb_host_uninstall() before reboot |
| Cold boot: hub not ready | Hub boots with ESP, not ready for enumeration. Fix: 10s retry reinstalls USB host stack |

## Zigbee2MQTT Integration

### Configuration (HA Addon)
```yaml
serial:
  port: tcp://192.168.1.108:8880   # TCP port from USB bridge web UI
  baudrate: 115200                  # Standard for Silicon Labs adapters
  adapter: ember                    # NOT 'ezsp' (deprecated in Z2M 2.x)
```

### Important Notes
- `adapter: ezsp` is deprecated → use `ember` for Silicon Labs (Sonoff ZBDongle-E, SkyConnect)
- `adapter: zstack` for Texas Instruments (CC2652-based dongles)
- Baudrate must match both Z2M config AND USB bridge config (both 115200)
- TCP port must match the port assigned in USB bridge web UI
- MQTT server must be `mqtt://core-mosquitto:1883` (not `localhost` — Docker networking)
- WiFi-based bridges: Z2M warns about packet loss. Wired Ethernet preferred.
- `adapter` field belongs in `/config/zigbee2mqtt/configuration.yaml`, NOT in the addon options UI
- Green Power devices (PTM 215ZE) require Zigbee channel 11, 15, 20, or 25

## Bugfix Session 2026-03-30

### Reported Issues
1. No device re-enumeration after reboot — devices not detected without unplugging
2. "Assigned" status jumped between SkyConnect and Sonoff
3. After Save & Reboot: wrong device got the mapping
4. TCP connection silent — Zigbee2MQTT could not communicate with stick
5. Debug log missing from web UI
6. Refresh button non-functional

### Root Causes & Fixes
1. **PHY reset too short** — SE0 from 100ms to 500ms, settle from 2.1s to 3s
2. **is_assigned matched by VID/PID instead of USB address** — now address-based, no confusion
3. **No serial number matching** — NVS v2 stores serial, matching uses VID+PID+Serial
4. **CDC-ACM missing SET_CONTROL_LINE_STATE** — DTR+RTS were not activated, stick stayed silent.
   Additionally: CDC default interface set to 1 (Data instead of Control), auto-fallback to other interfaces
5. **Ring buffer log (4KB)** — new endpoint `/api/usb/log`, panel in web UI, auto-refresh 10s
6. **Refresh overwrote local configs** — refresh now only fetches status, not config

### NVS Breaking Change
- NVS config version 1 → 2 (serial field added)
- Old v1 configs are automatically deleted on first boot
- User must reconfigure devices after this update

### CDC-ACM Init
```
SET_LINE_CODING: baud, 8N1
SET_CONTROL_LINE_STATE: DTR=1, RTS=1  ← THIS WAS MISSING
```
Without DTR/RTS activated, many CDC-ACM chips (CH9102, ATmega16U2) do not respond.

### Architecture Notes

#### 1) USB Device Re-enumeration After Reboot ✅
- **Goal**: Hub + devices must reliably re-enumerate after ESP reboot.
- **Final fix**: Clean USB stack shutdown before `esp_restart()` + cold boot retry after 10s.
- **Note**: GPIO SE0 approach was abandoned — does not work on ESP32-S3 internal PHY.

#### 2) Control Transfer Timeouts (CP210X/FTDI/CDC Init)
- **Root cause**: Control transfers called from USB client event callback → deadlock,
  because completion callbacks are only delivered via `usb_host_client_handle_events()`.
- **Fix**:
  - Event callback only queues device address — no `device_open()`, no transfers.
  - Device processing happens in monitor task (drains queue).
  - `ctrl_transfer_sync_()` pumps client events while waiting for completion.

#### 3) "Only first device connects" / `interface_claim() = ESP_ERR_NOT_SUPPORTED` ✅
- **Root cause**: ESP32-S3 has only **8 USB Host (HCD) channels**. Hub + multiple devices exhaust them quickly.
- **Practical limit**: With typical USB 2.0 hub, max **2 serial devices simultaneously**.
- **Fix**: `enum_filter_allow_all_()` accepts all devices. User selects via web UI.
- **IMPORTANT**: `CONFIG_USB_HOST_ENABLE_ENUM_FILTER_CALLBACK=y` must be set in sdkconfig,
  otherwise the `enum_filter_cb` field does not exist in `usb_host_config_t`.

#### 4) Debug Log: ESPHome Log + Web UI simultaneously
- ESPHome log via `ESP_LOGI/W/E`, plus ring buffer append for web UI (`/api/usb/log`).

## GitHub
- **Repo**: https://github.com/ESDN83/esp-ha-usb-gateway
- **Branch**: master

## References
- ESP-IDF USB issues: #10086, #12412, #9519, #13933, #17918
- SLZB-MR5U: ESP32-based USB passthrough with device selection UI
- HB-RF-ETH-ng: https://github.com/Xerolux/HB-RF-ETH-ng (Vue 3 web UI reference)
