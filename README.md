# ESP HA USB Gateway

ESP32-S3 USB-to-TCP bridge for Home Assistant. Bridges USB serial devices (Zigbee sticks, EnOcean, FTDI) over WiFi to TCP sockets — no USB passthrough needed. Enables **VM live migration** in Proxmox/ESXi environments.

## How It Works

```
USB Devices ←→ USB Hub ←→ ESP32-S3 (USB Host OTG) ←→ TCP Sockets ←→ Home Assistant
                           Web UI for config            port 8880+
```

- ESP32-S3 acts as USB Host, auto-detects all connected serial devices
- Each device gets its own TCP port (configured via web UI)
- Supports FTDI, CP210X (Silicon Labs), CDC-ACM chips
- Config stored in NVS flash — survives reboots

## Supported Devices

| Device | VID:PID | Chip | Tested |
|--------|---------|------|--------|
| SkyConnect v1.0 | `10C4:EA60` | CP210X | ✅ |
| Sonoff Zigbee 3.0 USB Dongle Plus V2 | `10C4:EA60` | CP210X | ✅ |
| EnOcean USB 300 | `0403:6001` | FTDI FT232 | ✅ |
| Generic FTDI/CP210X/CDC-ACM | various | auto-detect | ✅ |

**Tested Integrations:** ZHA, Zigbee2MQTT (2.9.1), EnOcean MQTT UI

## Features

- **Web UI** — auto-detect USB devices, assign TCP ports, configure settings
- **Password protection** — HTTP Basic Auth for web UI (TCP serial ports remain open)
- **IP whitelist** — per-device restriction of which hosts can connect to serial ports
- **MQTT with HA Auto Discovery** — bridge status, device count, uptime, firmware, restart button
- **Multiple bridges** — each bridge uses its ESPHome `friendly_name` for unique identification

## Hardware Requirements

- **ESP32-S3** dev board (e.g. ESP32-S3-DevKitC-1 or Waveshare ESP32-S3 ETH)
- **Powered USB hub** (ESP32-S3 OTG port provides no VBUS) or USB-C OTG splitter with PD
- USB serial device(s)

## Quick Start

1. Add to your ESPHome config as external component (see [Example Configs](#example-configs) below)

2. Flash via ESPHome Dashboard, then open `http://<ESP_IP>/`

3. Detected USB devices appear in the web UI — assign TCP ports, set baud rates, Save & Reboot

4. Connect your integration to `tcp://<ESP_IP>:<port>` (e.g. Zigbee2MQTT `serial.port`)

5. Optional: Set admin password, IP whitelist, and MQTT in the Settings section

## Example Configs

### WiFi (ESP32-S3-DevKitC-1)

```yaml
esphome:
  name: usb-bridge-z2m
  friendly_name: USB TCP Bridge Z2M
  platformio_options:
    board_build.flash_mode: dio

esp32:
  board: esp32-s3-devkitc-1
  variant: esp32s3
  framework:
    type: esp-idf
    version: recommended

external_components:
  - source:
      type: git
      url: https://github.com/ESDN83/esp-ha-usb-gateway
      ref: master
    components: [usb_bridge]
    refresh: 0s

wifi:
  ssid: !secret wifi_ssid
  password: !secret wifi_password

logger:
  hardware_uart: UART0  # CRITICAL: default USB_SERIAL_JTAG blocks USB OTG!

api:
  encryption:
    key: !secret api_encryption_key
ota:
  - platform: esphome
    password: !secret ota_password

usb_bridge:
```

### Ethernet (Waveshare ESP32-S3 ETH with W5500)

```yaml
esphome:
  name: usb-bridge-enocean-eth
  friendly_name: USB TCP Bridge Enocean ETH
  platformio_options:
    board_build.flash_mode: dio

esp32:
  board: esp32-s3-devkitc-1
  variant: esp32s3
  framework:
    type: esp-idf
    version: recommended

external_components:
  - source:
      type: git
      url: https://github.com/ESDN83/esp-ha-usb-gateway
      ref: master
    components: [usb_bridge]
    refresh: 0s

ethernet:
  type: W5500
  clk_pin: GPIO13
  mosi_pin: GPIO11
  miso_pin: GPIO12
  cs_pin: GPIO14
  interrupt_pin: GPIO10
  reset_pin: GPIO9
  manual_ip:
    static_ip: 192.168.1.122
    gateway: 192.168.1.1
    subnet: 255.255.255.0
    dns1: 192.168.1.90

logger:
  hardware_uart: UART0

api:
  encryption:
    key: !secret api_encryption_key
ota:
  - platform: esphome
    password: !secret ota_password

usb_bridge:
```

## Web UI

Built-in config interface at `http://<ESP_IP>/`:
- Auto-detected USB devices with manufacturer, product, serial number
- One-click device-to-TCP-port assignment
- Per-device IP whitelist (restrict which hosts can connect)
- Settings: admin password, MQTT with HA Auto Discovery
- Debug log viewer

## Zigbee2MQTT Example

```yaml
serial:
  port: tcp://192.168.1.108:8880
  baudrate: 115200
  adapter: ember
mqtt:
  server: mqtt://core-mosquitto:1883
```

## Known Limitations

- **ESP32-S3 has 8 HCD channels** — with a USB hub, max 2 serial devices simultaneously
- Single TCP client per port
- Config changes require reboot

## Architecture

| Task | Core | Priority | Purpose |
|------|------|----------|---------|
| `usb_lib` | 0 | 10 | USB host library event daemon |
| `usb_mon` | 1 | 5 | Device connect/disconnect handling |
| `tcp_srv` | 1 | 5 | TCP server per configured device |
| `usb_rx` | 1 | 6 | Bulk IN reader per connected device |

## License

MIT

## Credits

- [ESPHome](https://esphome.io/) framework
- [ESP-IDF](https://github.com/espressif/esp-idf) USB Host library
- Inspired by [HB-RF-ETH-ng](https://github.com/Xerolux/HB-RF-ETH-ng) and [SLZB-MR5U](https://smlight.tech/)
