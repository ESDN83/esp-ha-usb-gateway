# ESP-HA-USB-Gateway Development Notes

## Project Goal
Generic ESP32-S3 USB-to-TCP bridge for Home Assistant. Replaces USB passthrough
to Proxmox VMs, enabling VM live migration. Similar to SLZB-MR5U's USB passthrough
feature but as a standalone ESP32-S3 device.

## Hardware Setup

### ESP32-S3-DevKitC-1
- **IP**: 192.168.1.108
- **ESPHome name**: usb-bridge
- **Two USB-C ports**: UART (programming/power) and OTG (USB host)
- **CRITICAL: OTG port does NOT provide 5V VBUS** — connected USB devices get no
  power from the ESP32-S3. A **powered USB hub is REQUIRED** between the ESP32-S3
  and any USB device. Do NOT suggest direct connection or hardware modification.
- **UART port**: Connected to external power supply for stable operation
- **OTG port**: GPIO19 (D-) / GPIO20 (D+), used for USB host mode

### Powered USB Hub
- **VID=1A40 PID=0201** — generic USB 2.0 hub chip
- Required because ESP32-S3 OTG provides no VBUS
- Hub provides power to downstream USB devices
- Hub appears as first enumerated device (typically addr=1)

### Target USB Devices
| Device | VID:PID | Chip | Baud | Protocol |
|--------|---------|------|------|----------|
| EnOcean USB 300 | 0403:6001 | FTDI FT232 | 57600 | FTDI vendor requests |
| Sonoff Zigbee 3.0 V2 | 1A86:55D4 (CH9102) | CDC-ACM | 115200 | CDC SET_LINE_CODING |
| RFLink (Arduino Mega) | 2341:0042 | ATmega16U2 | 57600 | CDC-ACM |
| SkyConnect v1.0 | 10C4:EA60 | CP2102N | 115200 | CDC-ACM |

## Architecture

### ESPHome Component
- Path: `components/usb_bridge/`
- Python: `__init__.py` — config schema, sdkconfig options
- C++ header: `usb_bridge.h` — all implementation (single-header component)
- YAML: `esphome/usb-bridge.yaml` — device config (references GitHub repo)

### Tasks (FreeRTOS)
| Task | Stack | Core | Priority | Purpose |
|------|-------|------|----------|---------|
| usb_lib | 4096 | 0 | 10 | `usb_host_lib_handle_events()` — USB library daemon |
| usb_mon | 8192 | 1 | 5 | `usb_host_client_handle_events()` — device monitoring |
| tcp_srv | 8192 | 1 | 5 | TCP server, accepts clients, forwards data |
| usb_rx | 4096 | 1 | 6 | Bulk IN reads, forwards to TCP client |

### Data Flow
```
USB Device ←→ [Bulk IN/OUT] ←→ ESP32-S3 ←→ [TCP:8880] ←→ HA Add-on/Integration
```

### Chip Detection
1. **FTDI** (VID=0403): Vendor-specific control requests for baud/data/flow
2. **CDC-ACM** (interface class 0x02): Standard SET_LINE_CODING
3. **Generic**: Just find bulk endpoints, no initialization

## Key Technical Decisions

### Raw USB Host API Only
- No external IDF components (usb_host_cdc_acm, usb_host_vcp, etc.)
- All dropped due to version conflicts, compilation bugs, missing headers
- FTDI protocol implemented manually via vendor control requests
- CDC-ACM SET_LINE_CODING implemented manually

### Hub Support Approach
- ESP-IDF built-in hub driver handles hub enumeration (`CONFIG_USB_HOST_HUBS_SUPPORTED` and `MULTI_LEVEL`).
- Our code skips hub devices (class 0x09) — does not open/claim them, handing them completely to the ESP-IDF hub daemon.
- Downstream attached devices automatically natively trigger separate `NEW_DEV` events.

### Socket API
- Uses `lwip_socket()`, `lwip_send()`, etc. directly
- Cannot use `socket()` macro — conflicts with ESP-IDF's lwip headers

## sdkconfig Options
```
CONFIG_USB_OTG_SUPPORTED=y
CONFIG_USB_HOST_CONTROL_TRANSFER_MAX_SIZE=1024
CONFIG_SPIRAM=n                           # PSRAM breaks USB host (IDF #9519)
CONFIG_ESP32S3_SPIRAM_SUPPORT=n           # Both needed to fully disable PSRAM
```
Set in `__init__.py` via `add_idf_sdkconfig_option`. YAML `sdkconfig_options` has
the base USB OTG option only.

### Failed sdkconfig attempts (don't repeat!)
- `CONFIG_USB_HOST_HUBS_SUPPORTED` — may not exist in IDF 5.5.2
- `CONFIG_USB_HOST_HUB_MULTI_LEVEL` — didn't fix root port reset
- `CONFIG_USB_HOST_DEBOUNCE_DELAY_MS` / `RESET_HOLD_MS` etc. — didn't help
- `CONFIG_USB_HOST_ISR_IRAM_SAFE` — may not exist
- All timing-related options were ineffective

## ESP-IDF Version
- ESPHome 2026.1.0 uses **ESP-IDF 5.5.2** (`version: recommended`)
- Hub support available since ESP-IDF 5.2
- Multi-level hub support since ESP-IDF 5.4
- ESP32-S3 has only **8 USB host channels** (each endpoint = 1 channel)

## Known Issues & Fixes Log

### 1. lwip socket() macro conflict
- **Error**: `socket()` conflicts with lwip macro
- **Fix**: Use `lwip_socket()`, `lwip_send()`, `lwip_recv()` etc. directly

### 2. USB class macro conflicts
- **Error**: `USB_CLASS_CDC_DATA` / `USB_CLASS_HUB` already defined
- **Fix**: Only define `USB_CLASS_CDC` with `#ifndef` guard

### 3. FTDI FT232 class name
- **Error**: `FtdiDevice` not found
- **Fix**: Correct class is `esp_usb::FT23x` in esp_usb namespace (v1.0.0)
- **Later**: Dropped external component entirely, implemented FTDI manually

### 4. External IDF component version hell
- usb_host_vcp 1.0.0 vs usb_host_cdc_acm 2.0.0 conflict
- std::array incomplete type bug in ftdi_vcp v1.0.0 header
- Git repo override_path breaks ESP-USB monorepo internal references
- **Resolution**: Abandoned ALL external components, raw API only

### 5. ESPHome git cache not refreshing
- Even with `refresh: 0s`, code changes not picked up
- **Fix**: Must do "Clean Build Files" in ESPHome UI after code changes

### 6. Control transfer deadlock in event callback
- **Problem**: `handle_hub_()` called from `client_event_cb_()` inside
  `usb_host_client_handle_events()`. Control transfer completions also
  delivered through same function → deadlock, all transfers timeout.
- **Fix**: Defer hub handling via `pending_hub_addr_` flag, process in main `loop()` task.
- **Current status**: Custom hub driver permanently abandoned. Using ESP-IDF built-in hub driver now that underlying WDT/deadlocks are fixed.

### 7. Root port reset failed (2026-03-29)
- **Error**: `E (xxxxx) HUB: Root port reset failed` & `Interrupt wdt timeout on CPU0`
- **Source**: ESP-IDF's internal HUB component and Client Registration.
- **Root cause 1 (WDT/Reset Crash)**: `usb_host_client_register()` was called twice, leaking a client handle. The leaked client's event queue filled up with events, causing the USB daemon task to spin endlessly. Crucially, this blocked `event_pending` from clearing, meaning any `hcd_port_command(RESET)` (which ESP-IDF relies on for the hub) was instantly rejected.
- **Root cause 2 (No NEW_DEV for downstream)**: When abandoning the built-in driver, the ESP-IDF USB Host stack (without `CONFIG_USB_HOST_HUBS_SUPPORTED`) natively ignores downstream devices behind hubs. Returning to the built-in driver was the only way to get downstream `NEW_DEV` events.
- **Fix**: 
  - Fixed double client registration in `usb_task_()`.
  - Re-enabled `CONFIG_USB_HOST_HUBS_SUPPORTED`. The built-in driver now fully functions because the internal event deadlock is gone.

## Reference Projects
- **SLZB-MR5U**: ESP32-based, has USB passthrough with device/interface selection
  dropdown. Our target UX model. Screenshot available in previous conversations.
- **HB-RF-ETH-ng**: https://github.com/Xerolux/HB-RF-ETH-ng — Vue 3 SPA web UI,
  target for Phase 2 web interface design.

## Phase Plan
1. **Phase 1** (current): Basic USB-to-TCP bridge working with hub support
2. **Phase 2**: Web UI with device selection (like SLZB-MR5U)
   - Device dropdown showing all connected USB devices (VID, PID, description)
   - Interface selection for multi-interface devices
   - Baud rate configuration via UI
   - Vue 3 SPA like HB-RF-ETH-ng
3. **Phase 3**: Multiple simultaneous device support (multiple TCP ports)

## GitHub
- **Repo**: https://github.com/ESDN83/esp-ha-usb-gateway
- **Branch**: master
- **ESPHome external_components ref**: master

## ESPHome Deployment
1. Edit code locally in `D:\ai-projects\esp-ha-usb-gateway\`
2. `git commit && git push`
3. In HA ESPHome UI: **Clean Build Files** (critical after sdkconfig changes!)
4. Install/deploy to device
5. User has `refresh: 0s` locally on HA for development
