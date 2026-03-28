# ESP HA USB Gateway

An ESP32-S3 based USB-to-TCP bridge for Home Assistant, built with ESPHome.

Bridges any **FTDI-based USB serial device** (e.g. EnOcean USB 300, FTDI FT232) to a TCP socket over WiFi/Ethernet. This eliminates USB passthrough dependencies in virtualized environments (Proxmox, ESXi, etc.) and enables **VM live migration**.

## How It Works

```
USB Device ←→ ESP32-S3 USB Host (OTG) ←→ TCP Socket ←→ Home Assistant
              (FTDI FT232 driver)          (port 8880)
```

The ESP32-S3 acts as a USB Host via its native OTG port, communicates with the FTDI chip over USB, and exposes the serial data stream as a TCP server. Any application that supports `socket://` connections (e.g. EnOcean MQTT, ser2net clients) can connect.

## Supported Devices

Any USB serial device with an **FTDI FT232** chip, including:

| Device | VID:PID | Baud Rate | Tested |
|--------|---------|-----------|--------|
| EnOcean USB 300 | `0403:6001` | 57600 | Yes |
| Generic FTDI FT232 | `0403:6001` | configurable | - |

> **Note:** Standard CDC-ACM devices (e.g. Arduino, CP2102) are not yet supported. FTDI requires a vendor-specific USB driver which this component provides.

## Hardware Requirements

- **ESP32-S3** development board with **two USB-C ports** (one for flashing, one for USB Host OTG)
  - Tested: ESP32-S3-DevKitC-1
- USB serial device (FTDI-based)

### Wiring

The ESP32-S3 DevKitC-1 has two USB-C connectors:
- **USB** (GPIO19/20): USB OTG Host port → connect your USB serial device here
- **UART**: Programming/debug port → connect to PC for initial flash

## Installation

### Prerequisites

- [Home Assistant](https://www.home-assistant.io/) with [ESPHome](https://esphome.io/) add-on or [ESPHome Device Builder](https://esphome.io/guides/getting_started_hassio)
- ESP-IDF framework (selected automatically in the YAML config)

### Quick Start

1. Copy the `esphome/` folder contents to your ESPHome config directory:
   ```
   esphome/
   ├── components/
   │   └── usb_bridge/
   │       ├── __init__.py
   │       └── usb_bridge.h
   └── usb-bridge.yaml
   ```

2. Add your WiFi credentials to `secrets.yaml`:
   ```yaml
   wifi_ssid: "YourSSID"
   wifi_password: "YourPassword"
   api_encryption_key: "<generate with: openssl rand -base64 32>"
   ota_password: "your-ota-password"
   ```

3. Flash via ESPHome Dashboard or CLI:
   ```bash
   esphome run usb-bridge.yaml
   ```

4. Connect your USB device to the ESP32-S3 OTG port.

5. Configure your application to connect to `tcp:<ESP_IP>:8880`

### EnOcean USB 300 Example

After flashing, configure the [EnOcean MQTT Add-on](https://github.com/ESDN83/HA_enoceanmqtt-addon-ui) with:
```
tcp:192.168.1.108:8880
```

## Configuration

```yaml
usb_bridge:
  tcp_port: 8880      # TCP server port (default: 8880)
  baud_rate: 57600     # Serial baud rate (default: 57600)
```

## Architecture

The component runs three FreeRTOS tasks on the ESP32-S3:

| Task | Core | Purpose |
|------|------|---------|
| `usb_lib` | 0 | USB Host library event handling |
| `usb_mon` | 1 | USB device detection, connect/reconnect |
| `tcp_srv` | 1 | TCP server, bidirectional data forwarding |

- **USB → TCP**: Data received from USB triggers a callback that forwards directly to the TCP client
- **TCP → USB**: Data received via TCP `recv()` is forwarded to USB via `tx_blocking()`
- Thread safety via FreeRTOS mutex on VCP device access
- Automatic USB device reconnection on disconnect

## Web Interface

ESPHome provides a built-in web interface at `http://<ESP_IP>/` with:
- Device status and sensor readings
- Log output
- OTA firmware updates

## Use Cases

- **Proxmox / ESXi VM live migration** — Remove USB passthrough dependencies from VMs
- **Remote USB serial access** — Access USB devices over the network
- **Home Assistant integration** — Bridge USB dongles without direct host access

## Roadmap

- [ ] CDC-ACM device support (Arduino, CP2102, CH340)
- [ ] Multiple simultaneous USB devices
- [ ] Custom web UI with device status dashboard (like [HB-RF-ETH-ng](https://github.com/Xerolux/HB-RF-ETH-ng))
- [ ] USB device auto-detection with VID/PID filtering
- [ ] Ethernet support (ESP32-S3 + W5500/LAN8720)

## License

MIT

## Credits

- [ESPHome](https://esphome.io/) framework
- [Espressif USB Host components](https://components.espressif.com/) (CDC-ACM, VCP, FTDI)
- Inspired by [HB-RF-ETH-ng](https://github.com/Xerolux/HB-RF-ETH-ng) and [SLZB-MR5U](https://smlight.tech/)
