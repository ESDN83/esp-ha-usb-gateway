#pragma once

#include "esphome/core/component.h"
#include "esphome/core/log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"

#include "usb/usb_host.h"

#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "esp_netif.h"
#include "esp_system.h"

#include <atomic>
#include <cstring>
#include <vector>

#include "web_ui.h"

namespace esphome {
namespace usb_bridge {

static const char *const TAG = "usb_bridge";

// Known USB serial chip vendors
static constexpr uint16_t FTDI_VID = 0x0403;
static constexpr uint16_t CP210X_VID = 0x10C4;

// FTDI vendor requests
static constexpr uint8_t FTDI_REQ_RESET = 0x00;
static constexpr uint8_t FTDI_REQ_SET_FLOW_CTRL = 0x02;
static constexpr uint8_t FTDI_REQ_SET_BAUDRATE = 0x03;
static constexpr uint8_t FTDI_REQ_SET_DATA = 0x04;

// CP210X vendor requests
static constexpr uint8_t CP210X_IFC_ENABLE = 0x00;
static constexpr uint8_t CP210X_SET_MHS = 0x07;
static constexpr uint8_t CP210X_SET_BAUDRATE = 0x1E;

// CDC-ACM class requests
static constexpr uint8_t CDC_SET_LINE_CODING = 0x20;
static constexpr uint8_t CDC_SET_CONTROL_LINE_STATE = 0x22;

#ifndef USB_CLASS_CDC
#define USB_CLASS_CDC 0x02
#endif

enum class ChipType : uint8_t {
  UNKNOWN = 0,
  FTDI,
  CP210X,
  CDC_ACM,
  GENERIC,
};

static const char *chip_type_str(ChipType t) {
  switch (t) {
    case ChipType::FTDI: return "FTDI";
    case ChipType::CP210X: return "CP210X";
    case ChipType::CDC_ACM: return "CDC-ACM";
    case ChipType::GENERIC: return "Generic";
    default: return "Unknown";
  }
}

// ── Ring buffer debug log ──────────────────────────────────
static constexpr size_t LOG_RING_SIZE = 4096;
static char log_ring_[LOG_RING_SIZE];
static size_t log_ring_head_ = 0;
static SemaphoreHandle_t log_ring_mutex_ = nullptr;

static void log_ring_init_() {
  if (!log_ring_mutex_) log_ring_mutex_ = xSemaphoreCreateMutex();
  memset(log_ring_, 0, LOG_RING_SIZE);
  log_ring_head_ = 0;
}

static void log_ring_append_(const char *msg) {
  if (!log_ring_mutex_) return;
  xSemaphoreTake(log_ring_mutex_, portMAX_DELAY);
  size_t len = strlen(msg);
  for (size_t i = 0; i < len; i++) {
    log_ring_[log_ring_head_] = msg[i];
    log_ring_head_ = (log_ring_head_ + 1) % LOG_RING_SIZE;
  }
  // Add newline
  log_ring_[log_ring_head_] = '\n';
  log_ring_head_ = (log_ring_head_ + 1) % LOG_RING_SIZE;
  xSemaphoreGive(log_ring_mutex_);
}

static size_t log_ring_read_(char *out, size_t max_len) {
  if (!log_ring_mutex_) return 0;
  xSemaphoreTake(log_ring_mutex_, portMAX_DELAY);
  // Read from oldest to newest
  size_t written = 0;
  size_t pos = log_ring_head_;
  for (size_t i = 0; i < LOG_RING_SIZE && written < max_len - 1; i++) {
    char c = log_ring_[pos];
    if (c != 0) out[written++] = c;
    pos = (pos + 1) % LOG_RING_SIZE;
  }
  out[written] = 0;
  xSemaphoreGive(log_ring_mutex_);
  return written;
}

#define BRIDGE_LOG(fmt, ...) do { \
  char _buf[256]; \
  snprintf(_buf, sizeof(_buf), fmt, ##__VA_ARGS__); \
  ESP_LOGI(TAG, "%s", _buf); \
  log_ring_append_(_buf); \
} while(0)

#define BRIDGE_LOGW(fmt, ...) do { \
  char _buf[256]; \
  snprintf(_buf, sizeof(_buf), "WARN: " fmt, ##__VA_ARGS__); \
  ESP_LOGW(TAG, fmt, ##__VA_ARGS__); \
  log_ring_append_(_buf); \
} while(0)

#define BRIDGE_LOGE(fmt, ...) do { \
  char _buf[256]; \
  snprintf(_buf, sizeof(_buf), "ERR: " fmt, ##__VA_ARGS__); \
  ESP_LOGE(TAG, fmt, ##__VA_ARGS__); \
  log_ring_append_(_buf); \
} while(0)

// ── Discovered USB device (detected on bus) ─────────────────
struct DiscoveredDevice {
  uint8_t addr;
  uint16_t vid;
  uint16_t pid;
  uint8_t dev_class;
  uint8_t num_interfaces;
  bool is_hub;
  bool is_assigned;
  char manufacturer[64];
  char product[64];
  char serial[64];
};

// ── Saved device config (from NVS) ──────────────────────────
struct DeviceConfig {
  uint16_t vid;
  uint16_t pid;
  int port;
  int baud_rate;
  uint8_t interface;
  bool autoboot;
  char name[32];
  char serial[64];
};

// USB Hub class requests (USB 2.0 spec, Table 11-16)
static constexpr uint8_t USB_HUB_REQ_GET_DESCRIPTOR = 0x06;
static constexpr uint8_t USB_HUB_REQ_CLEAR_FEATURE = 0x01;
static constexpr uint8_t USB_HUB_REQ_SET_FEATURE = 0x03;
// Hub port features
static constexpr uint16_t USB_HUB_FEAT_PORT_POWER = 8;
static constexpr uint16_t USB_HUB_FEAT_PORT_RESET = 4;

class UsbBridgeComponent;

struct DeviceConnection {
  UsbBridgeComponent *parent{nullptr};
  DeviceConfig config{};

  usb_device_handle_t dev_hdl{nullptr};
  uint8_t dev_addr{0};
  uint8_t bulk_in_ep{0};
  uint8_t bulk_out_ep{0};
  uint16_t bulk_in_mps{64};
  uint8_t claimed_intf{0};
  ChipType chip_type{ChipType::UNKNOWN};

  std::atomic<bool> connected{false};
  std::atomic<int> tcp_client_fd{-1};
  SemaphoreHandle_t usb_mutex{nullptr};
  SemaphoreHandle_t bulk_in_sem{nullptr};
};

class UsbBridgeComponent : public Component {
 public:
  std::vector<DeviceConnection*>& get_connections() { return connections_; }
  std::vector<DiscoveredDevice>& get_discovered() { return discovered_; }
  SemaphoreHandle_t get_discovery_mutex() { return discovery_mutex_; }

  float get_setup_priority() const override {
    return setup_priority::AFTER_WIFI;
  }

  void setup() override {
    log_ring_init_();
    BRIDGE_LOG("USB Gateway initializing...");

    discovery_mutex_ = xSemaphoreCreateMutex();
    instance_ = this;
    ctrl_xfer_done_ = xSemaphoreCreateBinary();

    // ── USB PHY reset ────────────────────────────────────────
    BRIDGE_LOG("USB PHY reset: SE0 on GPIO19/20 for 500ms...");
    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pin_bit_mask = (1ULL << 19) | (1ULL << 20);
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    gpio_config(&io_conf);
    gpio_set_level(GPIO_NUM_19, 0);
    gpio_set_level(GPIO_NUM_20, 0);
    vTaskDelay(pdMS_TO_TICKS(500));
    io_conf.mode = GPIO_MODE_INPUT;
    gpio_config(&io_conf);
    vTaskDelay(pdMS_TO_TICKS(1000));

    // ── Install USB Host + register client ───────────────────
    const usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
    };
    esp_err_t err = usb_host_install(&host_config);
    if (err != ESP_OK) {
      BRIDGE_LOGE("USB Host install failed: %s", esp_err_to_name(err));
      this->mark_failed();
      return;
    }

    const usb_host_client_config_t client_config = {
        .is_synchronous = false,
        .max_num_event_msg = 5,
        .async = {
            .client_event_callback = client_event_cb_,
            .callback_arg = this,
        },
    };
    err = usb_host_client_register(&client_config, &client_hdl_);
    if (err != ESP_OK) {
      BRIDGE_LOGE("Client register failed: %s", esp_err_to_name(err));
      this->mark_failed();
      return;
    }

    // Load saved device configs from NVS
    load_nvs_config_();
    BRIDGE_LOG("Loaded %zu saved device configs from NVS", connections_.size());

    // Start USB lib daemon + monitor task
    xTaskCreatePinnedToCore(usb_lib_task_, "usb_lib", 8192, nullptr, 10, nullptr, 0);
    xTaskCreatePinnedToCore(usb_task_entry_, "usb_mon", 8192, this, 5, nullptr, 1);

    // Launch TCP servers for saved configs
    for (auto *conn : connections_) {
      xTaskCreatePinnedToCore(tcp_task_entry_, "tcp_srv", 8192, conn, 5, nullptr, 1);
    }

    // Start config web UI on port 80
    web_ctx_.component = this;
    web_ctx_.server = start_config_webserver_(80);

    BRIDGE_LOG("USB TCP Gateway ready — config UI at http://<ip>/");
  }

  void loop() override {}

  // Called from web UI to save config and reboot
  bool save_config_and_reboot(const std::vector<StoredDeviceConfig> &devs) {
    if (nvs_save_devices(devs)) {
      BRIDGE_LOG("Config saved, rebooting in 1s...");
      vTaskDelay(pdMS_TO_TICKS(1000));
      esp_restart();
      return true;
    }
    return false;
  }

 private:
  static UsbBridgeComponent *instance_;

  std::vector<DeviceConnection*> connections_;
  std::vector<DiscoveredDevice> discovered_;
  SemaphoreHandle_t discovery_mutex_{nullptr};

  usb_host_client_handle_t client_hdl_{nullptr};

  SemaphoreHandle_t ctrl_xfer_done_{nullptr};
  usb_transfer_status_t ctrl_xfer_status_{};
  uint16_t ctrl_xfer_actual_{0};

  // Hub power cycle state
  uint8_t hub_addr_{0};
  uint8_t hub_num_ports_{0};
  bool hub_power_cycle_done_{false};

  // ── Convert USB string descriptor (UTF-16LE) to ASCII ──────
  static void str_desc_to_ascii_(const usb_str_desc_t *desc, char *out, size_t max_len) {
    out[0] = 0;
    if (!desc || desc->bLength < 4) return;
    size_t num_chars = (desc->bLength - 2) / 2;
    if (num_chars >= max_len) num_chars = max_len - 1;
    for (size_t i = 0; i < num_chars; i++) {
      uint16_t wchar = desc->wData[i];
      out[i] = (wchar < 128) ? (char)wchar : '?';
    }
    out[num_chars] = 0;
  }

  // ── Load config from NVS ──────────────────────────────────
  void load_nvs_config_() {
    std::vector<StoredDeviceConfig> stored;
    if (!nvs_load_devices(stored)) {
      BRIDGE_LOG("No saved config in NVS — configure via web UI");
      return;
    }

    for (auto &s : stored) {
      DeviceConnection *conn = new DeviceConnection();
      conn->config.vid = s.vid;
      conn->config.pid = s.pid;
      conn->config.port = s.port;
      conn->config.baud_rate = s.baud_rate;
      conn->config.interface = s.interface;
      conn->config.autoboot = s.autoboot;
      strncpy(conn->config.name, s.name, sizeof(conn->config.name) - 1);
      strncpy(conn->config.serial, s.serial, sizeof(conn->config.serial) - 1);
      conn->parent = this;
      conn->usb_mutex = xSemaphoreCreateMutex();
      connections_.push_back(conn);
      BRIDGE_LOG("  Config: %s VID=%04X PID=%04X S/N=%s port=%d baud=%d",
               s.name, s.vid, s.pid, s.serial[0] ? s.serial : "(none)", s.port, s.baud_rate);
    }
  }

  // ── USB Host Library daemon ───────────────────────────────
  static void usb_lib_task_(void *arg) {
    while (true) {
      uint32_t event_flags = 0;
      esp_err_t err = usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
      if (err != ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(100));
      }
    }
  }

  // ── USB client event callback ─────────────────────────────
  static void client_event_cb_(const usb_host_client_event_msg_t *event_msg,
                                void *arg) {
    auto *self = static_cast<UsbBridgeComponent *>(arg);
    switch (event_msg->event) {
      case USB_HOST_CLIENT_EVENT_NEW_DEV:
        BRIDGE_LOG(">>> New USB device event (addr=%d)", event_msg->new_dev.address);
        self->handle_new_device_(event_msg->new_dev.address);
        break;
      case USB_HOST_CLIENT_EVENT_DEV_GONE:
        BRIDGE_LOGW("<<< USB device removed event");
        self->close_device_(event_msg->dev_gone.dev_hdl);
        break;
      default:
        break;
    }
  }

  // ── Handle new device: discover + auto-assign if configured ─
  void handle_new_device_(uint8_t dev_addr) {
    usb_device_handle_t dev;
    esp_err_t err = usb_host_device_open(client_hdl_, dev_addr, &dev);
    if (err != ESP_OK) {
      BRIDGE_LOGE("Failed to open device addr=%d: %s", dev_addr, esp_err_to_name(err));
      return;
    }

    const usb_device_desc_t *desc;
    err = usb_host_get_device_descriptor(dev, &desc);
    if (err != ESP_OK) {
      BRIDGE_LOGE("Failed to get descriptor addr=%d: %s", dev_addr, esp_err_to_name(err));
      usb_host_device_close(client_hdl_, dev);
      return;
    }

    bool is_hub = (desc->bDeviceClass == USB_CLASS_HUB);

    BRIDGE_LOG("Device addr=%d: VID=%04X PID=%04X Class=%02X%s",
             dev_addr, desc->idVendor, desc->idProduct, desc->bDeviceClass,
             is_hub ? " [HUB]" : "");

    // Read device string descriptors
    usb_device_info_t dev_info = {};
    usb_host_device_info(dev, &dev_info);

    DiscoveredDevice dd = {};
    dd.addr = dev_addr;
    dd.vid = desc->idVendor;
    dd.pid = desc->idProduct;
    dd.dev_class = desc->bDeviceClass;
    dd.is_hub = is_hub;
    dd.is_assigned = false;

    str_desc_to_ascii_(dev_info.str_desc_manufacturer, dd.manufacturer, sizeof(dd.manufacturer));
    str_desc_to_ascii_(dev_info.str_desc_product, dd.product, sizeof(dd.product));
    str_desc_to_ascii_(dev_info.str_desc_serial_num, dd.serial, sizeof(dd.serial));

    BRIDGE_LOG("  Manufacturer: %s", dd.manufacturer[0] ? dd.manufacturer : "(none)");
    BRIDGE_LOG("  Product: %s", dd.product[0] ? dd.product : "(none)");
    BRIDGE_LOG("  Serial: %s", dd.serial[0] ? dd.serial : "(none)");

    const usb_config_desc_t *config_desc = nullptr;
    if (!is_hub) {
      usb_host_get_active_config_descriptor(dev, &config_desc);
      if (config_desc) {
        dd.num_interfaces = config_desc->bNumInterfaces;
        BRIDGE_LOG("  Interfaces: %d, TotalLength: %d", config_desc->bNumInterfaces, config_desc->wTotalLength);
      }
    }

    // Add to discovered list (replace existing entry for same addr)
    xSemaphoreTake(discovery_mutex_, portMAX_DELAY);
    for (auto it = discovered_.begin(); it != discovered_.end(); ) {
      if (it->addr == dev_addr) it = discovered_.erase(it);
      else ++it;
    }
    discovered_.push_back(dd);
    xSemaphoreGive(discovery_mutex_);

    if (is_hub) {
      BRIDGE_LOG("  Hub detected at addr=%d — power-cycling ports...", dev_addr);
      hub_addr_ = dev_addr;

      // Read hub descriptor to learn number of ports
      uint8_t hub_desc_buf[16] = {};
      esp_err_t herr = ctrl_transfer_sync_(dev,
          USB_BM_REQUEST_TYPE_DIR_IN | USB_BM_REQUEST_TYPE_TYPE_CLASS | USB_BM_REQUEST_TYPE_RECIP_DEVICE,
          USB_HUB_REQ_GET_DESCRIPTOR, (0x29 << 8) | 0, 0, sizeof(hub_desc_buf), hub_desc_buf);

      uint8_t num_ports = 4;
      if (herr == ESP_OK && hub_desc_buf[0] >= 3) {
        num_ports = hub_desc_buf[2];
        BRIDGE_LOG("  Hub has %d downstream ports", num_ports);
      } else {
        BRIDGE_LOGW("  Hub descriptor read failed (%s), assuming %d ports", esp_err_to_name(herr), num_ports);
      }
      hub_num_ports_ = num_ports;

      // Only do power cycle on first boot (not on re-enumeration after our own cycle)
      if (!hub_power_cycle_done_) {
        hub_power_cycle_done_ = true;
        BRIDGE_LOG("  Power-cycling %d hub ports...", num_ports);

        // Power OFF all ports
        for (uint8_t port = 1; port <= num_ports; port++) {
          herr = ctrl_transfer_sync_(dev,
              USB_BM_REQUEST_TYPE_DIR_OUT | USB_BM_REQUEST_TYPE_TYPE_CLASS | USB_BM_REQUEST_TYPE_RECIP_OTHER,
              USB_HUB_REQ_CLEAR_FEATURE, USB_HUB_FEAT_PORT_POWER, port, 0, nullptr);
          BRIDGE_LOG("  Port %d power OFF: %s", port, esp_err_to_name(herr));
        }

        vTaskDelay(pdMS_TO_TICKS(2000));

        // Power ON all ports
        for (uint8_t port = 1; port <= num_ports; port++) {
          herr = ctrl_transfer_sync_(dev,
              USB_BM_REQUEST_TYPE_DIR_OUT | USB_BM_REQUEST_TYPE_TYPE_CLASS | USB_BM_REQUEST_TYPE_RECIP_OTHER,
              USB_HUB_REQ_SET_FEATURE, USB_HUB_FEAT_PORT_POWER, port, 0, nullptr);
          BRIDGE_LOG("  Port %d power ON: %s", port, esp_err_to_name(herr));
        }
        BRIDGE_LOG("  Hub port power cycle complete — devices will re-enumerate");
      } else {
        BRIDGE_LOG("  Hub already power-cycled, skipping");
      }

      usb_host_device_close(client_hdl_, dev);
      return;
    }

    // Try to match against saved configs (VID + PID + optional serial)
    DeviceConnection *target = nullptr;
    for (auto *conn : connections_) {
      bool vid_match = (conn->config.vid == desc->idVendor);
      bool pid_match = (conn->config.pid == desc->idProduct);
      bool serial_match = true;
      // If config has a serial number, require it to match
      if (conn->config.serial[0] != '\0' && dd.serial[0] != '\0') {
        serial_match = (strcmp(conn->config.serial, dd.serial) == 0);
      }
      if (vid_match && pid_match && serial_match && !conn->connected.load()) {
        target = conn;
        BRIDGE_LOG("  Matched config: %s (port=%d)", conn->config.name, conn->config.port);
        break;
      }
    }

    if (!target) {
      BRIDGE_LOG("  No matching config — available in web UI");
      usb_host_device_close(client_hdl_, dev);
      return;
    }

    // Assign device
    if (!config_desc) {
      err = usb_host_get_active_config_descriptor(dev, &config_desc);
      if (err != ESP_OK) {
        BRIDGE_LOGE("  Failed to get config descriptor: %s", esp_err_to_name(err));
        usb_host_device_close(client_hdl_, dev);
        return;
      }
    }

    target->chip_type = detect_chip_type_(desc, config_desc);
    BRIDGE_LOG("  Chip type: %s", chip_type_str(target->chip_type));

    if (!find_bulk_endpoints_(target, config_desc)) {
      BRIDGE_LOGW("  No bulk endpoints on intf %d — trying other interfaces...", target->config.interface);
      // Try to find bulk endpoints on any interface
      bool found = false;
      for (uint8_t intf = 0; intf < config_desc->bNumInterfaces && !found; intf++) {
        if (intf == target->config.interface) continue;
        target->config.interface = intf;
        if (find_bulk_endpoints_(target, config_desc)) {
          BRIDGE_LOG("  Found bulk endpoints on intf %d", intf);
          found = true;
        }
      }
      if (!found) {
        BRIDGE_LOGE("  No bulk endpoints on any interface!");
        usb_host_device_close(client_hdl_, dev);
        return;
      }
    }
    BRIDGE_LOG("  Bulk IN: EP 0x%02X (MPS=%d), Bulk OUT: EP 0x%02X, Intf: %d",
             target->bulk_in_ep, target->bulk_in_mps, target->bulk_out_ep, target->claimed_intf);

    err = usb_host_interface_claim(client_hdl_, dev, target->claimed_intf, 0);
    if (err != ESP_OK) {
      BRIDGE_LOGE("  interface_claim(%d) failed: %s", target->claimed_intf, esp_err_to_name(err));
      usb_host_device_close(client_hdl_, dev);
      return;
    }

    xSemaphoreTake(target->usb_mutex, portMAX_DELAY);
    target->dev_hdl = dev;
    target->dev_addr = dev_addr;
    xSemaphoreGive(target->usb_mutex);

    // Chip-specific initialization
    if (target->chip_type == ChipType::FTDI) {
      BRIDGE_LOG("  Initializing FTDI (%d baud)...", target->config.baud_rate);
      ftdi_init_(target);
    } else if (target->chip_type == ChipType::CP210X) {
      BRIDGE_LOG("  Initializing CP210X (%d baud, autoboot=%d)...",
               target->config.baud_rate, target->config.autoboot);
      cp210x_init_(target);
    } else if (target->chip_type == ChipType::CDC_ACM) {
      BRIDGE_LOG("  Initializing CDC-ACM (%d baud)...", target->config.baud_rate);
      cdc_acm_init_(target);
    }

    target->connected.store(true);

    // Mark as assigned in discovery list (by address, not VID/PID)
    xSemaphoreTake(discovery_mutex_, portMAX_DELAY);
    for (auto &d : discovered_) {
      if (d.addr == dev_addr) {
        d.is_assigned = true;
        break;
      }
    }
    xSemaphoreGive(discovery_mutex_);

    BRIDGE_LOG("=== ASSIGNED %s (VID=%04X PID=%04X addr=%d) -> TCP port %d ===",
             target->config.name, desc->idVendor, desc->idProduct,
             dev_addr, target->config.port);

    xTaskCreatePinnedToCore(bulk_read_task_entry_, "usb_rx", 4096,
                            target, 6, nullptr, 1);
  }

  // ── Synchronous control transfer ──────────────────────────
  static void ctrl_xfer_sync_cb_(usb_transfer_t *xfer) {
    auto *self = static_cast<UsbBridgeComponent *>(xfer->context);
    self->ctrl_xfer_status_ = xfer->status;
    self->ctrl_xfer_actual_ = xfer->actual_num_bytes;
    xSemaphoreGive(self->ctrl_xfer_done_);
  }

  esp_err_t ctrl_transfer_sync_(usb_device_handle_t dev,
                                 uint8_t bmRequestType, uint8_t bRequest,
                                 uint16_t wValue, uint16_t wIndex,
                                 uint16_t wLength, uint8_t *data_out = nullptr) {
    usb_transfer_t *xfer;
    size_t alloc_size = sizeof(usb_setup_packet_t) + wLength;
    esp_err_t err = usb_host_transfer_alloc(alloc_size, 0, &xfer);
    if (err != ESP_OK) return err;

    usb_setup_packet_t *setup = (usb_setup_packet_t *)xfer->data_buffer;
    setup->bmRequestType = bmRequestType;
    setup->bRequest = bRequest;
    setup->wValue = wValue;
    setup->wIndex = wIndex;
    setup->wLength = wLength;

    xfer->device_handle = dev;
    xfer->bEndpointAddress = 0;
    xfer->callback = ctrl_xfer_sync_cb_;
    xfer->context = this;
    xfer->num_bytes = sizeof(usb_setup_packet_t) + wLength;
    xfer->timeout_ms = 2000;

    xSemaphoreTake(ctrl_xfer_done_, 0);
    err = usb_host_transfer_submit_control(client_hdl_, xfer);
    if (err != ESP_OK) { usb_host_transfer_free(xfer); return err; }

    if (xSemaphoreTake(ctrl_xfer_done_, pdMS_TO_TICKS(3000)) != pdTRUE) {
      usb_host_transfer_free(xfer); return ESP_ERR_TIMEOUT;
    }
    if (ctrl_xfer_status_ != USB_TRANSFER_STATUS_COMPLETED) {
      usb_host_transfer_free(xfer); return ESP_FAIL;
    }
    if ((bmRequestType & USB_BM_REQUEST_TYPE_DIR_IN) && data_out && wLength > 0) {
      size_t copy_len = ctrl_xfer_actual_ > sizeof(usb_setup_packet_t)
                        ? ctrl_xfer_actual_ - sizeof(usb_setup_packet_t) : 0;
      if (copy_len > wLength) copy_len = wLength;
      memcpy(data_out, xfer->data_buffer + sizeof(usb_setup_packet_t), copy_len);
    }
    usb_host_transfer_free(xfer);
    return ESP_OK;
  }

  esp_err_t ctrl_transfer_sync_out_(usb_device_handle_t dev,
                                 uint8_t bmRequestType, uint8_t bRequest,
                                 uint16_t wValue, uint16_t wIndex,
                                 uint16_t wLength, const uint8_t *data_in = nullptr) {
    usb_transfer_t *xfer;
    size_t alloc_size = sizeof(usb_setup_packet_t) + wLength;
    esp_err_t err = usb_host_transfer_alloc(alloc_size, 0, &xfer);
    if (err != ESP_OK) return err;

    usb_setup_packet_t *setup = (usb_setup_packet_t *)xfer->data_buffer;
    setup->bmRequestType = bmRequestType;
    setup->bRequest = bRequest;
    setup->wValue = wValue;
    setup->wIndex = wIndex;
    setup->wLength = wLength;

    if (wLength > 0 && data_in)
      memcpy(xfer->data_buffer + sizeof(usb_setup_packet_t), data_in, wLength);

    xfer->device_handle = dev;
    xfer->bEndpointAddress = 0;
    xfer->callback = ctrl_xfer_sync_cb_;
    xfer->context = this;
    xfer->num_bytes = sizeof(usb_setup_packet_t) + wLength;
    xfer->timeout_ms = 2000;

    xSemaphoreTake(ctrl_xfer_done_, 0);
    err = usb_host_transfer_submit_control(client_hdl_, xfer);
    if (err != ESP_OK) { usb_host_transfer_free(xfer); return err; }

    if (xSemaphoreTake(ctrl_xfer_done_, pdMS_TO_TICKS(3000)) != pdTRUE) {
      usb_host_transfer_free(xfer); return ESP_ERR_TIMEOUT;
    }
    if (ctrl_xfer_status_ != USB_TRANSFER_STATUS_COMPLETED) {
      usb_host_transfer_free(xfer); return ESP_FAIL;
    }
    usb_host_transfer_free(xfer);
    return ESP_OK;
  }

  // ── Detect chip type ──────────────────────────────────────
  ChipType detect_chip_type_(const usb_device_desc_t *desc,
                             const usb_config_desc_t *config_desc) {
    if (desc->idVendor == FTDI_VID) return ChipType::FTDI;
    if (desc->idVendor == CP210X_VID) return ChipType::CP210X;

    int offset = 0;
    for (int i = 0; i < config_desc->bNumInterfaces; i++) {
      const usb_intf_desc_t *intf = usb_parse_interface_descriptor(config_desc, i, 0, &offset);
      if (!intf) continue;
      if (intf->bInterfaceClass == USB_CLASS_CDC || intf->bInterfaceClass == USB_CLASS_CDC_DATA)
        return ChipType::CDC_ACM;
    }
    return ChipType::GENERIC;
  }

  // ── Find bulk endpoints (manual descriptor walk) ──────────
  bool find_bulk_endpoints_(DeviceConnection *conn, const usb_config_desc_t *config_desc) {
    conn->bulk_in_ep = 0;
    conn->bulk_out_ep = 0;

    const uint8_t* p = (const uint8_t*)config_desc;
    const uint8_t* end = p + config_desc->wTotalLength;
    uint8_t current_intf = 0;
    uint8_t requested_intf = conn->config.interface;

    while (p < end && p[0] >= 2) {
      uint8_t len = p[0], type = p[1];
      if (type == USB_B_DESCRIPTOR_TYPE_INTERFACE) {
        current_intf = ((const usb_intf_desc_t*)p)->bInterfaceNumber;
      } else if (type == USB_B_DESCRIPTOR_TYPE_ENDPOINT && current_intf == requested_intf) {
        const usb_ep_desc_t* ep = (const usb_ep_desc_t*)p;
        if ((ep->bmAttributes & USB_BM_ATTRIBUTES_XFERTYPE_MASK) == USB_BM_ATTRIBUTES_XFER_BULK) {
          if (ep->bEndpointAddress & 0x80) {
            conn->bulk_in_ep = ep->bEndpointAddress;
            conn->bulk_in_mps = ep->wMaxPacketSize;
          } else {
            conn->bulk_out_ep = ep->bEndpointAddress;
          }
        }
      }
      p += len;
    }

    if (conn->bulk_in_ep && conn->bulk_out_ep) {
      conn->claimed_intf = requested_intf;
      return true;
    }
    return false;
  }

  // ── Chip initialization ───────────────────────────────────
  void cp210x_init_(DeviceConnection *conn) {
    auto vc = [&](uint8_t req, uint16_t val, uint16_t idx, uint16_t wlen, const uint8_t *data) -> esp_err_t {
      return ctrl_transfer_sync_out_(conn->dev_hdl,
          USB_BM_REQUEST_TYPE_DIR_OUT | USB_BM_REQUEST_TYPE_TYPE_VENDOR |
              USB_BM_REQUEST_TYPE_RECIP_INTERFACE,
          req, val, idx, wlen, data);
    };
    esp_err_t err;
    err = vc(CP210X_IFC_ENABLE, 1, conn->config.interface, 0, nullptr);
    BRIDGE_LOG("    CP210X IFC_ENABLE: %s", esp_err_to_name(err));

    if (!conn->config.autoboot) {
      err = vc(CP210X_SET_MHS, 0x0303, conn->config.interface, 0, nullptr);
      BRIDGE_LOG("    CP210X SET_MHS (DTR+RTS): %s", esp_err_to_name(err));
    } else {
      err = vc(CP210X_SET_MHS, 0x0300, conn->config.interface, 0, nullptr);
      BRIDGE_LOG("    CP210X SET_MHS (no DTR/RTS toggle): %s", esp_err_to_name(err));
    }

    uint32_t baud = conn->config.baud_rate;
    err = vc(CP210X_SET_BAUDRATE, 0, conn->config.interface, 4, (const uint8_t*)&baud);
    BRIDGE_LOG("    CP210X SET_BAUDRATE(%d): %s", baud, esp_err_to_name(err));
  }

  void ftdi_init_(DeviceConnection *conn) {
    auto vc = [&](uint8_t req, uint16_t val, uint16_t idx) -> esp_err_t {
      return ctrl_transfer_sync_(conn->dev_hdl,
          USB_BM_REQUEST_TYPE_DIR_OUT | USB_BM_REQUEST_TYPE_TYPE_VENDOR |
              USB_BM_REQUEST_TYPE_RECIP_DEVICE,
          req, val, idx, 0, nullptr);
    };
    esp_err_t err;
    err = vc(FTDI_REQ_RESET, 0, conn->config.interface);   // Reset
    BRIDGE_LOG("    FTDI RESET: %s", esp_err_to_name(err));
    err = vc(FTDI_REQ_RESET, 1, conn->config.interface);   // Purge RX
    err = vc(FTDI_REQ_RESET, 2, conn->config.interface);   // Purge TX

    uint16_t baud_val = (conn->config.baud_rate > 0) ? (3000000 / conn->config.baud_rate) : 26;
    err = vc(FTDI_REQ_SET_BAUDRATE, baud_val, conn->config.interface);
    BRIDGE_LOG("    FTDI SET_BAUDRATE(val=%d -> %d baud): %s", baud_val, conn->config.baud_rate, esp_err_to_name(err));

    err = vc(FTDI_REQ_SET_DATA, 0x0008, conn->config.interface);  // 8N1
    BRIDGE_LOG("    FTDI SET_DATA(8N1): %s", esp_err_to_name(err));
    err = vc(FTDI_REQ_SET_FLOW_CTRL, 0, conn->config.interface);  // No flow control
    BRIDGE_LOG("    FTDI SET_FLOW_CTRL(none): %s", esp_err_to_name(err));

    if (!conn->config.autoboot) {
      // Toggle DTR+RTS for devices that need it
      vc(0x01, 0x0101, conn->config.interface);
      vc(0x01, 0x0202, conn->config.interface);
      BRIDGE_LOG("    FTDI DTR+RTS toggled");
    }
  }

  void cdc_acm_init_(DeviceConnection *conn) {
    // SET_LINE_CODING: baud, stop bits, parity, data bits
    uint8_t data[7];
    uint32_t baud = conn->config.baud_rate;
    memcpy(data, &baud, 4);
    data[4] = 0;  // 1 stop bit
    data[5] = 0;  // no parity
    data[6] = 8;  // 8 data bits
    esp_err_t err = ctrl_transfer_sync_out_(conn->dev_hdl,
        USB_BM_REQUEST_TYPE_DIR_OUT | USB_BM_REQUEST_TYPE_TYPE_CLASS | USB_BM_REQUEST_TYPE_RECIP_INTERFACE,
        CDC_SET_LINE_CODING, 0, conn->config.interface, 7, data);
    BRIDGE_LOG("    CDC SET_LINE_CODING(%d 8N1): %s", baud, esp_err_to_name(err));

    // SET_CONTROL_LINE_STATE: activate DTR + RTS
    err = ctrl_transfer_sync_out_(conn->dev_hdl,
        USB_BM_REQUEST_TYPE_DIR_OUT | USB_BM_REQUEST_TYPE_TYPE_CLASS | USB_BM_REQUEST_TYPE_RECIP_INTERFACE,
        CDC_SET_CONTROL_LINE_STATE, 0x0003, conn->config.interface, 0, nullptr);
    BRIDGE_LOG("    CDC SET_CONTROL_LINE_STATE(DTR+RTS): %s", esp_err_to_name(err));
  }

  // ── Close USB device ──────────────────────────────────────
  void close_device_(usb_device_handle_t dev) {
    for (auto *conn : connections_) {
      if (conn->dev_hdl == dev) {
        uint8_t addr = conn->dev_addr;
        BRIDGE_LOGW("Device disconnected: %s (addr=%d, port=%d)", conn->config.name, addr, conn->config.port);
        conn->connected.store(false);
        xSemaphoreTake(conn->usb_mutex, portMAX_DELAY);
        usb_host_interface_release(client_hdl_, dev, conn->claimed_intf);
        usb_host_device_close(client_hdl_, dev);
        conn->dev_hdl = nullptr;
        conn->dev_addr = 0;
        xSemaphoreGive(conn->usb_mutex);
        conn->chip_type = ChipType::UNKNOWN;

        // Remove from discovery list by address
        xSemaphoreTake(discovery_mutex_, portMAX_DELAY);
        for (auto it = discovered_.begin(); it != discovered_.end(); ) {
          if (it->addr == addr) it = discovered_.erase(it);
          else ++it;
        }
        xSemaphoreGive(discovery_mutex_);
        break;
      }
    }
  }

  // ── Bulk read: USB -> TCP ─────────────────────────────────
  static void bulk_read_task_entry_(void *arg) {
    static_cast<DeviceConnection *>(arg)->parent->bulk_read_task_(
        static_cast<DeviceConnection *>(arg));
  }

  void bulk_read_task_(DeviceConnection *conn) {
    usb_transfer_t *xfer;
    esp_err_t err = usb_host_transfer_alloc(conn->bulk_in_mps, 0, &xfer);
    if (err != ESP_OK) {
      BRIDGE_LOGE("Bulk IN alloc failed for port %d", conn->config.port);
      vTaskDelete(nullptr);
      return;
    }

    xfer->device_handle = conn->dev_hdl;
    xfer->bEndpointAddress = conn->bulk_in_ep;
    xfer->callback = bulk_in_cb_;
    xfer->context = conn;
    xfer->num_bytes = conn->bulk_in_mps;
    xfer->timeout_ms = 1000;
    conn->bulk_in_sem = xSemaphoreCreateBinary();

    BRIDGE_LOG("Bulk read task started for %s (port %d)", conn->config.name, conn->config.port);

    uint32_t rx_bytes = 0;
    uint32_t rx_errors = 0;
    while (conn->connected.load()) {
      err = usb_host_transfer_submit(xfer);
      if (err != ESP_OK) {
        if (conn->connected.load()) {
          rx_errors++;
          BRIDGE_LOGW("Bulk IN submit failed port %d: %s (errors=%u)", conn->config.port, esp_err_to_name(err), rx_errors);
        }
        break;
      }
      xSemaphoreTake(conn->bulk_in_sem, portMAX_DELAY);
    }

    BRIDGE_LOG("Bulk read task ended for %s (rx=%u bytes, errors=%u)", conn->config.name, rx_bytes, rx_errors);
    usb_host_transfer_free(xfer);
    if (conn->bulk_in_sem) { vSemaphoreDelete(conn->bulk_in_sem); conn->bulk_in_sem = nullptr; }
    vTaskDelete(nullptr);
  }

  static void bulk_in_cb_(usb_transfer_t *xfer) {
    auto *conn = static_cast<DeviceConnection *>(xfer->context);
    if (xfer->status == USB_TRANSFER_STATUS_COMPLETED && xfer->actual_num_bytes > 0) {
      const uint8_t *data = xfer->data_buffer;
      size_t len = xfer->actual_num_bytes;
      // FTDI prepends 2 modem status bytes per packet
      if (conn->chip_type == ChipType::FTDI && len > 2) { data += 2; len -= 2; }
      int fd = conn->tcp_client_fd.load();
      if (fd >= 0 && len > 0) lwip_send(fd, data, len, MSG_DONTWAIT);
    }
    if (conn->bulk_in_sem) xSemaphoreGiveFromISR(conn->bulk_in_sem, nullptr);
  }

  // ── USB monitor task ──────────────────────────────────────
  static void usb_task_entry_(void *arg) {
    static_cast<UsbBridgeComponent *>(arg)->usb_task_();
  }
  void usb_task_() {
    while (true) {
      usb_host_client_handle_events(client_hdl_, pdMS_TO_TICKS(1000));
    }
  }

  // ── TCP -> USB write ──────────────────────────────────────
  void usb_write_(DeviceConnection *conn, const uint8_t *data, size_t len) {
    xSemaphoreTake(conn->usb_mutex, portMAX_DELAY);
    if (!conn->dev_hdl || !conn->connected.load()) {
      xSemaphoreGive(conn->usb_mutex); return;
    }
    usb_transfer_t *xfer;
    if (usb_host_transfer_alloc(len, 0, &xfer) != ESP_OK) {
      xSemaphoreGive(conn->usb_mutex); return;
    }
    memcpy(xfer->data_buffer, data, len);
    xfer->device_handle = conn->dev_hdl;
    xfer->bEndpointAddress = conn->bulk_out_ep;
    xfer->callback = bulk_out_cb_;
    xfer->context = nullptr;
    xfer->num_bytes = len;
    xfer->timeout_ms = 1000;
    if (usb_host_transfer_submit(xfer) != ESP_OK) usb_host_transfer_free(xfer);
    xSemaphoreGive(conn->usb_mutex);
  }

  static void bulk_out_cb_(usb_transfer_t *xfer) {
    if (xfer->status != USB_TRANSFER_STATUS_COMPLETED)
      ESP_LOGW(TAG, "Bulk OUT failed: status=%d", xfer->status);
    usb_host_transfer_free(xfer);
  }

  // ── TCP server per device ─────────────────────────────────
  static void tcp_task_entry_(void *arg) {
    auto *conn = static_cast<DeviceConnection *>(arg);
    conn->parent->tcp_task_(conn);
  }

  void tcp_task_(DeviceConnection *conn) {
    vTaskDelay(pdMS_TO_TICKS(3000));
    while (true) {
      int server_fd = lwip_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
      if (server_fd < 0) {
        BRIDGE_LOGE("TCP socket create failed for port %d", conn->config.port);
        vTaskDelay(pdMS_TO_TICKS(5000));
        continue;
      }

      int opt = 1;
      lwip_setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

      struct sockaddr_in addr = {};
      addr.sin_family = AF_INET;
      addr.sin_addr.s_addr = INADDR_ANY;
      addr.sin_port = htons(conn->config.port);

      if (lwip_bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
          lwip_listen(server_fd, 1) < 0) {
        BRIDGE_LOGE("TCP bind/listen failed for port %d", conn->config.port);
        lwip_close(server_fd);
        vTaskDelay(pdMS_TO_TICKS(5000));
        continue;
      }

      BRIDGE_LOG("TCP server listening on port %d for %s", conn->config.port, conn->config.name);

      while (true) {
        struct sockaddr_in ca; socklen_t cl = sizeof(ca);
        int cfd = lwip_accept(server_fd, (struct sockaddr *)&ca, &cl);
        if (cfd < 0) break;

        char as[INET_ADDRSTRLEN];
        inet_ntoa_r(ca.sin_addr, as, sizeof(as));
        BRIDGE_LOG("TCP client connected on port %d from %s (device: %s, usb_connected=%d)",
                 conn->config.port, as, conn->config.name, conn->connected.load());

        opt = 1;
        lwip_setsockopt(cfd, SOL_SOCKET, SO_KEEPALIVE, &opt, sizeof(opt));
        // Set TCP_NODELAY for low latency
        lwip_setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
        conn->tcp_client_fd.store(cfd);

        uint8_t buf[256];
        while (true) {
          int len = lwip_recv(cfd, buf, sizeof(buf), 0);
          if (len <= 0) break;
          if (conn->connected.load()) usb_write_(conn, buf, len);
        }

        BRIDGE_LOG("TCP client disconnected on port %d", conn->config.port);
        conn->tcp_client_fd.store(-1);
        lwip_close(cfd);
      }
      lwip_close(server_fd);
    }
  }
};

UsbBridgeComponent *UsbBridgeComponent::instance_ = nullptr;

// ── HTTP Handler Implementations ────────────────────────────

static esp_err_t handle_get_config_(httpd_req_t *req) {
  auto *comp = web_ctx_.component;
  if (!comp) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No component"); return ESP_FAIL; }

  auto &conns = comp->get_connections();
  char buf[2048];
  char *p = buf;
  const char *end = buf + sizeof(buf) - 2;
  *p++ = '[';
  for (size_t i = 0; i < conns.size() && p < end - 200; i++) {
    auto &cfg = conns[i]->config;
    if (i > 0) *p++ = ',';
    *p++ = '{';
    json_append_str(p, end, "name", cfg.name);
    json_append_int(p, end, "vid", cfg.vid);
    json_append_int(p, end, "pid", cfg.pid);
    json_append_int(p, end, "port", cfg.port);
    json_append_int(p, end, "baud_rate", cfg.baud_rate);
    json_append_int(p, end, "interface", cfg.interface);
    json_append_bool(p, end, "autoboot", cfg.autoboot);
    json_append_str(p, end, "serial", cfg.serial, false);
    *p++ = '}';
  }
  *p++ = ']'; *p = 0;
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, buf, p - buf);
  return ESP_OK;
}

static esp_err_t handle_get_status_(httpd_req_t *req) {
  auto *comp = web_ctx_.component;
  if (!comp) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No component"); return ESP_FAIL; }

  auto &conns = comp->get_connections();
  auto &disc = comp->get_discovered();

  char buf[3072];
  char *p = buf;
  const char *end = buf + sizeof(buf) - 2;

  p += snprintf(p, end - p, "{\"configured\":[");
  for (size_t i = 0; i < conns.size() && p < end - 100; i++) {
    if (i > 0) *p++ = ',';
    p += snprintf(p, end - p, "{\"connected\":%s,\"port\":%d,\"chip\":\"%s\"}",
                  conns[i]->connected.load() ? "true" : "false",
                  conns[i]->config.port,
                  chip_type_str(conns[i]->chip_type));
  }
  p += snprintf(p, end - p, "],\"discovered\":[");

  xSemaphoreTake(comp->get_discovery_mutex(), portMAX_DELAY);
  for (size_t i = 0; i < disc.size() && p < end - 350; i++) {
    if (i > 0) *p++ = ',';
    p += snprintf(p, end - p,
        "{\"addr\":%d,\"vid\":%d,\"pid\":%d,\"class\":%d,\"hub\":%s,\"assigned\":%s,"
        "\"interfaces\":%d,\"manufacturer\":\"%s\",\"product\":\"%s\",\"serial\":\"%s\"}",
        disc[i].addr, disc[i].vid, disc[i].pid, disc[i].dev_class,
        disc[i].is_hub ? "true" : "false",
        disc[i].is_assigned ? "true" : "false",
        disc[i].num_interfaces,
        disc[i].manufacturer, disc[i].product, disc[i].serial);
  }
  xSemaphoreGive(comp->get_discovery_mutex());

  p += snprintf(p, end - p, "]}");
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, buf, p - buf);
  return ESP_OK;
}

static esp_err_t handle_get_log_(httpd_req_t *req) {
  char *buf = (char *)malloc(LOG_RING_SIZE + 1);
  if (!buf) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM"); return ESP_FAIL; }
  size_t len = log_ring_read_(buf, LOG_RING_SIZE);
  httpd_resp_set_type(req, "text/plain");
  httpd_resp_send(req, buf, len);
  free(buf);
  return ESP_OK;
}

static esp_err_t handle_post_config_(httpd_req_t *req) {
  auto *comp = web_ctx_.component;
  if (!comp) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No component"); return ESP_FAIL; }

  int total_len = req->content_len;
  if (total_len > 4096) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Too large"); return ESP_FAIL; }

  char *body = (char *)malloc(total_len + 1);
  if (!body) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM"); return ESP_FAIL; }

  int received = 0;
  while (received < total_len) {
    int ret = httpd_req_recv(req, body + received, total_len - received);
    if (ret <= 0) { free(body); return ESP_FAIL; }
    received += ret;
  }
  body[total_len] = 0;

  std::vector<StoredDeviceConfig> devs;
  const char *ptr = body;
  while (*ptr) {
    const char *obj_start = strchr(ptr, '{');
    if (!obj_start) break;
    const char *obj_end = strchr(obj_start, '}');
    if (!obj_end) break;

    char obj[512];
    size_t obj_len = obj_end - obj_start + 1;
    if (obj_len >= sizeof(obj)) obj_len = sizeof(obj) - 1;
    memcpy(obj, obj_start, obj_len);
    obj[obj_len] = 0;

    StoredDeviceConfig cfg = {};
    json_get_str(obj, "name", cfg.name, sizeof(cfg.name));
    json_get_str(obj, "serial", cfg.serial, sizeof(cfg.serial));
    cfg.vid = json_get_int(obj, "vid", 0);
    cfg.pid = json_get_int(obj, "pid", 0);
    cfg.port = json_get_int(obj, "port", 8880);
    cfg.baud_rate = json_get_int(obj, "baud_rate", 115200);
    cfg.interface = json_get_int(obj, "interface", 0);
    cfg.autoboot = json_get_bool(obj, "autoboot", false) ? 1 : 0;

    if (cfg.vid > 0 && cfg.pid > 0) {
      devs.push_back(cfg);
      BRIDGE_LOG("POST config: %s VID=%04X PID=%04X S/N=%s port=%d baud=%d intf=%d autoboot=%d",
               cfg.name, cfg.vid, cfg.pid, cfg.serial[0] ? cfg.serial : "(none)",
               cfg.port, cfg.baud_rate, cfg.interface, cfg.autoboot);
    }
    ptr = obj_end + 1;
  }
  free(body);

  if (devs.empty()) {
    nvs_save_devices(devs);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"message\":\"Config cleared! Rebooting...\",\"ok\":true}");
  } else {
    nvs_save_devices(devs);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"message\":\"Config saved! Rebooting...\",\"ok\":true}");
  }

  xTaskCreate([](void *) { vTaskDelay(pdMS_TO_TICKS(1500)); esp_restart(); },
              "reboot", 2048, nullptr, 1, nullptr);
  return ESP_OK;
}

}  // namespace usb_bridge
}  // namespace esphome
