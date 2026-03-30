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
1. ESP32 boots, resets USB PHY (GPIO19/20 SE0), installs USB host
2. Built-in ESP-IDF hub driver enumerates hub + downstream devices
3. Each detected device appears in `discovered_` list
4. If a NVS-saved config matches (VID/PID), device is auto-assigned to TCP port
5. Unconfigured devices show as "Available" in web UI at port 81
6. User clicks "+ Configure", sets TCP port/baud/etc., clicks "Save & Reboot"
7. Config is saved to NVS flash, device reboots, devices auto-connect

### FreeRTOS Tasks
| Task | Stack | Core | Priority | Purpose |
|------|-------|------|----------|---------|
| usb_lib | 8192 | 0 | 10 | USB library event daemon |
| usb_mon | 8192 | 1 | 5 | Client event handler (device connect/disconnect) |
| tcp_srv | 8192 | 1 | 5 | TCP server per configured device |
| usb_rx | 4096 | 1 | 6 | Bulk IN reader per connected device |

### Web UI (port 81)
- **Detected Devices**: auto-populated from USB bus scan
- **Configured Mappings**: saved device-to-TCP-port assignments
- REST API: `GET/POST /api/usb/config`, `GET /api/usb/status`
- Config stored in NVS flash (survives reboots, no YAML needed)
- Save & Reboot applies changes

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
CONFIG_SPIRAM=n                    # PSRAM breaks USB host (IDF #9519)
CONFIG_ESP32S3_SPIRAM_SUPPORT=n
CONFIG_HTTPD_MAX_REQ_HDR_LEN=1024  # For config web UI
```

## ESP-IDF Version
- ESPHome 2026.1.0 uses **ESP-IDF 5.5.2** (`version: recommended`)
- Hub support since ESP-IDF 5.2
- ESP32-S3: only **8 USB host channels** (hub + 2 devices can exhaust them)

## Root Port Reset Fix
The "Root port reset failed" error was caused by:
1. **No USB PHY reset at boot** — hub already connected confuses ESP-IDF.
   Fix: drive GPIO19/20 LOW for 100ms before `usb_host_install()`.
2. **Client registration race** — lib task processing before client registered.
   Fix: register client in `setup()` before starting tasks.
3. **Built-in hub driver conflict** — custom hub code conflicted.
   Fix: let built-in driver handle hubs, skip hub class devices.

## Known Limitations
- **8 HCD channels**: ESP32-S3 limit. Hub + 2 serial devices is the practical max.
- **No hot-add of configs**: changing config requires reboot (Save & Reboot).
- **Single TCP client per port**: one client at a time per device.

## Solved Issues Reference
| Issue | Fix |
|-------|-----|
| Root port reset failed | GPIO PHY reset + client registration order |
| Control transfer deadlock | Don't do transfers inside event callback |
| WDT crash | Single client registration in setup() |
| Missing endpoints | Manual descriptor walking instead of ESP-IDF parser |
| lwip socket() conflict | Use lwip_socket() etc. directly |
| ESPHome git cache | Clean Build Files required after changes |
| PSRAM + USB conflict | Disable PSRAM via sdkconfig |

## GitHub
- **Repo**: https://github.com/ESDN83/esp-ha-usb-gateway
- **Branch**: master

## References
- ESP-IDF USB issues: #10086, #12412, #9519, #13933, #17918
- SLZB-MR5U: ESP32-based USB passthrough with device selection UI
- HB-RF-ETH-ng: https://github.com/Xerolux/HB-RF-ETH-ng (Vue 3 web UI reference)



## Bugfix Session 2026-03-30

### User-reported issues (29.03.2026)
1. Power cycle nach reboot fehlte — Devices nicht erkannt ohne Abstecken
2. "Assigned" Status sprang zwischen SkyConnect und Sonoff hin und her
3. Nach Save & Reboot: falsches Device bekam das Mapping
4. TCP-Verbindung stumm — Zigbee2MQTT konnte Stick nicht ansprechen
5. Debug-Log im Web-UI fehlte
6. Refresh-Button ohne Funktion

### Root Causes & Fixes
1. **PHY Reset zu kurz** — SE0 von 100ms auf 500ms, settle von 2.1s auf 3s
2. **is_assigned per VID/PID statt USB-Adresse** — jetzt Adress-basiert, kein Verwechseln
3. **Kein Serial-Number-Matching** — NVS v2 speichert Serial, Matching nutzt VID+PID+Serial
4. **CDC-ACM fehlte SET_CONTROL_LINE_STATE** — DTR+RTS wurden nicht aktiviert, Stick blieb stumm.
   Zusätzlich: CDC Default-Interface auf 1 (Data statt Control), auto-fallback auf andere Interfaces
5. **Ring-Buffer Log (4KB)** — neuer Endpoint `/api/usb/log`, Panel im Web-UI, auto-refresh 10s
6. **Refresh überschrieb lokale Configs** — Refresh holt jetzt nur Status, nicht Config

### NVS Breaking Change
- NVS Config Version 1 → 2 (serial field added)
- Alte v1 configs werden beim ersten Start automatisch gelöscht
- User muss Devices neu konfigurieren nach diesem Update

### CDC-ACM Init (neu)
```
SET_LINE_CODING: baud, 8N1
SET_CONTROL_LINE_STATE: DTR=1, RTS=1  ← DAS FEHLTE!
```
Ohne DTR/RTS aktiviert antworten viele CDC-ACM Chips (CH9102, ATmega16U2) nicht.