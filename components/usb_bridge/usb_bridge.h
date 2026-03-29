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

struct DeviceConfig {
  uint16_t vid;
  uint16_t pid;
  int port;
  int baud_rate;
  uint8_t interface;
  bool autoboot;
  char name[32];
};

class UsbBridgeComponent; // forward declare

struct DeviceConnection {
  UsbBridgeComponent *parent{nullptr};
  DeviceConfig config;
  
  usb_device_handle_t dev_hdl{nullptr};
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
  void add_device_config(const std::string &name, uint16_t vid, uint16_t pid, int port, int baud_rate, uint8_t interface, bool autoboot) {
    DeviceConnection *conn = new DeviceConnection();
    conn->config.vid = vid;
    conn->config.pid = pid;
    conn->config.port = port;
    conn->config.baud_rate = baud_rate;
    conn->config.interface = interface;
    conn->config.autoboot = autoboot;
    strncpy(conn->config.name, name.c_str(), sizeof(conn->config.name) - 1);
    conn->config.name[sizeof(conn->config.name) - 1] = 0;
    conn->parent = this;
    conn->usb_mutex = xSemaphoreCreateMutex();
    connections_.push_back(conn);
  }

  std::vector<DeviceConnection*>& get_connections() { return connections_; }

  float get_setup_priority() const override {
    return setup_priority::AFTER_WIFI;
  }

  void setup() override {
    ESP_LOGI(TAG, "USB Gateway initializing (%zu devices configured)", connections_.size());

    // Hardware USB PHY Reset to force enumeration of already connected hubs
    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pin_bit_mask = (1ULL << 19) | (1ULL << 20); // D- and D+ pins
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    gpio_config(&io_conf);
    
    // Drive both lines LOW for 100ms (forces USB reset SE0 state)
    gpio_set_level(GPIO_NUM_19, 0);
    gpio_set_level(GPIO_NUM_20, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    
    // Revert pins to input floating mode
    io_conf.mode = GPIO_MODE_INPUT;
    gpio_config(&io_conf);
    vTaskDelay(pdMS_TO_TICKS(100));

    // Wait 2 secs for completely drained hubs to spin back up
    vTaskDelay(pdMS_TO_TICKS(2000));

    instance_ = this;
    ctrl_xfer_done_ = xSemaphoreCreateBinary();

    const usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
    };
    esp_err_t err = usb_host_install(&host_config);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "USB Host install failed: %s", esp_err_to_name(err));
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
      ESP_LOGE(TAG, "Client register failed: %s", esp_err_to_name(err));
      this->mark_failed();
      return;
    }

    ESP_LOGI(TAG, "USB host client registered");

    xTaskCreatePinnedToCore(usb_lib_task_, "usb_lib", 8192,
                            nullptr, 10, nullptr, 0);
    xTaskCreatePinnedToCore(usb_task_entry_, "usb_mon", 8192,
                            this, 5, nullptr, 1);

    // Load NVS config — overrides YAML defaults if present
    load_nvs_config_();

    // Launch TCP servers for all configs
    for (auto *conn : connections_) {
      xTaskCreatePinnedToCore(tcp_task_entry_, "tcp_srv", 8192,
                              conn, 5, nullptr, 1);
    }

    // Start config web UI on port 81
    web_ctx_.component = this;
    web_ctx_.server = start_config_webserver_(81);

    ESP_LOGI(TAG, "USB TCP Gateway fully constructed");
  }

  void loop() override {}

  // Save current config to NVS and reboot
  bool save_and_reboot(const std::vector<StoredDeviceConfig> &devs) {
    if (nvs_save_devices(devs)) {
      ESP_LOGI(TAG, "Config saved, rebooting in 1s...");
      vTaskDelay(pdMS_TO_TICKS(1000));
      esp_restart();
      return true;
    }
    return false;
  }

 private:
  void load_nvs_config_() {
    std::vector<StoredDeviceConfig> stored;
    if (!nvs_load_devices(stored)) {
      ESP_LOGI(TAG, "No NVS config found, using YAML defaults");
      return;
    }

    ESP_LOGI(TAG, "Loading %zu device configs from NVS (overriding YAML)", stored.size());

    // Clear YAML-defined connections
    for (auto *conn : connections_) {
      delete conn;
    }
    connections_.clear();

    // Rebuild from NVS
    for (auto &s : stored) {
      DeviceConnection *conn = new DeviceConnection();
      conn->config = {s.vid, s.pid, s.port, (int)s.baud_rate, s.interface, (bool)s.autoboot};
      conn->parent = this;
      conn->usb_mutex = xSemaphoreCreateMutex();
      connections_.push_back(conn);
      ESP_LOGI(TAG, "  NVS device: %s VID=%04X PID=%04X port=%d baud=%d",
               s.name, s.vid, s.pid, s.port, s.baud_rate);
    }
  }

  static UsbBridgeComponent *instance_;

  std::vector<DeviceConnection*> connections_;

  usb_host_client_handle_t client_hdl_{nullptr};

  // Synchronous control transfer support
  SemaphoreHandle_t ctrl_xfer_done_{nullptr};
  usb_transfer_status_t ctrl_xfer_status_{};
  uint16_t ctrl_xfer_actual_{0};

  // ── USB Host Library daemon ───────────────────────────────
  static void usb_lib_task_(void *arg) {
    while (true) {
      uint32_t event_flags = 0;
      esp_err_t err = usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
      if (err != ESP_OK) {
        ESP_LOGE(TAG, "lib_handle_events failed: %d (%s)", err, esp_err_to_name(err));
        vTaskDelay(pdMS_TO_TICKS(100));
      } else {
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
          ESP_LOGI(TAG, "All clients deregistered, freeing devices...");
          usb_host_device_free_all();
        }
      }
    }
  }

  // ── USB client event callback ─────────────────────────────
  static void client_event_cb_(const usb_host_client_event_msg_t *event_msg,
                                void *arg) {
    auto *self = static_cast<UsbBridgeComponent *>(arg);
    switch (event_msg->event) {
      case USB_HOST_CLIENT_EVENT_NEW_DEV:
        ESP_LOGI(TAG, "New USB device (addr=%d)", event_msg->new_dev.address);
        self->try_open_device_(event_msg->new_dev.address);
        break;
      case USB_HOST_CLIENT_EVENT_DEV_GONE:
        ESP_LOGW(TAG, "USB device removed");
        self->close_device_(event_msg->dev_gone.dev_hdl);
        break;
      default:
        break;
    }
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
    if (err != ESP_OK) {
      usb_host_transfer_free(xfer);
      return err;
    }

    if (xSemaphoreTake(ctrl_xfer_done_, pdMS_TO_TICKS(3000)) != pdTRUE) {
      usb_host_transfer_free(xfer);
      return ESP_ERR_TIMEOUT;
    }

    if (ctrl_xfer_status_ != USB_TRANSFER_STATUS_COMPLETED) {
      usb_host_transfer_free(xfer);
      return ESP_FAIL;
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

  // Helper with OUT data support
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

    if (wLength > 0 && data_in) {
      memcpy(xfer->data_buffer + sizeof(usb_setup_packet_t), data_in, wLength);
    }

    xfer->device_handle = dev;
    xfer->bEndpointAddress = 0;
    xfer->callback = ctrl_xfer_sync_cb_;
    xfer->context = this;
    xfer->num_bytes = sizeof(usb_setup_packet_t) + wLength;
    xfer->timeout_ms = 2000;

    xSemaphoreTake(ctrl_xfer_done_, 0);

    err = usb_host_transfer_submit_control(client_hdl_, xfer);
    if (err != ESP_OK) {
      usb_host_transfer_free(xfer);
      return err;
    }

    if (xSemaphoreTake(ctrl_xfer_done_, pdMS_TO_TICKS(3000)) != pdTRUE) {
      usb_host_transfer_free(xfer);
      return ESP_ERR_TIMEOUT;
    }

    if (ctrl_xfer_status_ != USB_TRANSFER_STATUS_COMPLETED) {
      usb_host_transfer_free(xfer);
      return ESP_FAIL;
    }

    usb_host_transfer_free(xfer);
    return ESP_OK;
  }


// ── Detect chip type from device descriptor ───────────────
  ChipType detect_chip_type_(const usb_device_desc_t *desc,
                             const usb_config_desc_t *config_desc) {
    if (desc->idVendor == FTDI_VID) {
      ESP_LOGI(TAG, "Detected FTDI chip");
      return ChipType::FTDI;
    }
    if (desc->idVendor == CP210X_VID) {
      ESP_LOGI(TAG, "Detected CP210X chip");
      return ChipType::CP210X;
    }

    int offset = 0;
    for (int i = 0; i < config_desc->bNumInterfaces; i++) {
      const usb_intf_desc_t *intf = usb_parse_interface_descriptor(
          config_desc, i, 0, &offset);
      if (!intf) continue;
      if (intf->bInterfaceClass == USB_CLASS_CDC ||
          intf->bInterfaceClass == USB_CLASS_CDC_DATA) {
        ESP_LOGI(TAG, "Detected CDC-ACM device");
        return ChipType::CDC_ACM;
      }
    }

    ESP_LOGI(TAG, "Unknown USB serial device, using generic mode");
    return ChipType::GENERIC;
  }

  // ── Find bulk IN and OUT endpoints ────────────────────────
  bool find_bulk_endpoints_(DeviceConnection *conn, const usb_config_desc_t *config_desc) {
    conn->bulk_in_ep = 0;
    conn->bulk_out_ep = 0;

    const uint8_t* p = (const uint8_t*)config_desc;
    const uint8_t* end = p + config_desc->wTotalLength;

    uint8_t current_intf = 0;
    uint8_t requested_intf = conn->config.interface;

    while (p < end && p[0] >= 2) {
      uint8_t len = p[0];
      uint8_t type = p[1];

      if (type == USB_B_DESCRIPTOR_TYPE_INTERFACE) {
        const usb_intf_desc_t* intf = (const usb_intf_desc_t*)p;
        current_intf = intf->bInterfaceNumber;
      } else if (type == USB_B_DESCRIPTOR_TYPE_ENDPOINT) {
        if (current_intf == requested_intf) {
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
      }
      p += len;
    }

    if (conn->bulk_in_ep != 0 && conn->bulk_out_ep != 0) {
      conn->claimed_intf = requested_intf;
      ESP_LOGI(TAG, "Found bulk endpoints on intf %d", requested_intf);
      return true;
    }
    return false;
  }

  // ── Try to open any USB device ────────────────────────────
  void try_open_device_(uint8_t dev_addr) {
    usb_device_handle_t dev;
    esp_err_t err = usb_host_device_open(client_hdl_, dev_addr, &dev);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "device_open failed: %s", esp_err_to_name(err));
      return;
    }

    const usb_device_desc_t *desc;
    err = usb_host_get_device_descriptor(dev, &desc);
    if (err != ESP_OK) {
      usb_host_device_close(client_hdl_, dev);
      return;
    }

    if (desc->bDeviceClass == USB_CLASS_HUB) {
      ESP_LOGI(TAG, "USB hub detected, yielding to built-in driver...");
      usb_host_device_close(client_hdl_, dev);
      return;
    }

    // Identify which defined config matches this device
    DeviceConnection *target_conn = nullptr;
    for (auto *conn : connections_) {
      if (conn->config.vid == desc->idVendor && conn->config.pid == desc->idProduct) {
        if (!conn->connected.load()) {
          target_conn = conn;
          break; // First unmatched valid slot!
        }
      }
    }

    if (!target_conn) {
      ESP_LOGI(TAG, "Ignoring device %04X:%04X (not configured or all slots busy)", desc->idVendor, desc->idProduct);
      usb_host_device_close(client_hdl_, dev);
      return;
    }

    const usb_config_desc_t *config_desc;
    err = usb_host_get_active_config_descriptor(dev, &config_desc);
    if (err != ESP_OK) {
      usb_host_device_close(client_hdl_, dev);
      return;
    }

    target_conn->chip_type = detect_chip_type_(desc, config_desc);

    if (!find_bulk_endpoints_(target_conn, config_desc)) {
      ESP_LOGW(TAG, "No bulk endpoints found on intf %d", target_conn->config.interface);
      usb_host_device_close(client_hdl_, dev);
      return;
    }

    err = usb_host_interface_claim(client_hdl_, dev, target_conn->claimed_intf, 0);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "interface_claim failed: %s", esp_err_to_name(err));
      usb_host_device_close(client_hdl_, dev);
      return;
    }

    xSemaphoreTake(target_conn->usb_mutex, portMAX_DELAY);
    target_conn->dev_hdl = dev;
    xSemaphoreGive(target_conn->usb_mutex);

    // Hardware Initialization protocol
    if (target_conn->chip_type == ChipType::FTDI) {
      ftdi_init_(target_conn);
    } else if (target_conn->chip_type == ChipType::CP210X) {
      cp210x_init_(target_conn);
    } else if (target_conn->chip_type == ChipType::CDC_ACM) {
      cdc_set_line_coding_(target_conn);
    }

    target_conn->connected.store(true);

    ESP_LOGI(TAG, "USB device firmly assigned to Port %d: (VID=%04X PID=%04X, %d baud)",
             target_conn->config.port, desc->idVendor, desc->idProduct, target_conn->config.baud_rate);

    // Start read task for this specific connection
    xTaskCreatePinnedToCore(bulk_read_task_entry_, "usb_rx", 4096,
                            target_conn, 6, nullptr, 1);
  }

  // ── CP210X initialization ───────────────────────────────────
  void cp210x_init_(DeviceConnection *conn) {
    auto vc = [&](uint8_t req, uint16_t val, uint16_t idx, uint16_t wlen, const uint8_t *data) {
      ctrl_transfer_sync_out_(conn->dev_hdl,
          USB_BM_REQUEST_TYPE_DIR_OUT | USB_BM_REQUEST_TYPE_TYPE_VENDOR |
              USB_BM_REQUEST_TYPE_RECIP_INTERFACE,
          req, val, idx, wlen, data);
    };

    // IFC_ENABLE
    vc(CP210X_IFC_ENABLE, 1, conn->config.interface, 0, nullptr);

    // SET_MHS (DTR/RTS)
    if (!conn->config.autoboot) {
      vc(CP210X_SET_MHS, 0x0303, conn->config.interface, 0, nullptr); // Assert DTR/RTS
    } else {
      vc(CP210X_SET_MHS, 0x0300, conn->config.interface, 0, nullptr); // Safe state avoiding autoboot
    }

    // SET_BAUDRATE
    uint32_t baud = conn->config.baud_rate;
    vc(CP210X_SET_BAUDRATE, 0, conn->config.interface, 4, (const uint8_t*)&baud);
    
    ESP_LOGI(TAG, "CP210x successfully configured (baud=%d, autoboot=%d)", baud, conn->config.autoboot);
  }

  // ── FTDI initialization ───────────────────────────────────
  void ftdi_init_(DeviceConnection *conn) {
    auto vc = [&](uint8_t req, uint16_t val, uint16_t idx) {
      ctrl_transfer_sync_(conn->dev_hdl,
          USB_BM_REQUEST_TYPE_DIR_OUT | USB_BM_REQUEST_TYPE_TYPE_VENDOR |
              USB_BM_REQUEST_TYPE_RECIP_DEVICE,
          req, val, idx, 0, nullptr);
    };
    vc(FTDI_REQ_RESET, 0, conn->config.interface); // Reset device
    vc(FTDI_REQ_RESET, 1, conn->config.interface); // Clear RX
    vc(FTDI_REQ_RESET, 2, conn->config.interface); // Clear TX
    
    uint32_t baud = conn->config.baud_rate;
    // Approximated FTDI divisor logic. 
    vc(FTDI_REQ_SET_BAUDRATE, 3000000 / baud, conn->config.interface);
    vc(FTDI_REQ_SET_DATA, 0x0008, conn->config.interface);
    vc(FTDI_REQ_SET_FLOW_CTRL, 0, conn->config.interface);

    if (!conn->config.autoboot) {
      vc(0x01, 0x0101, conn->config.interface); // Assert DTR
      vc(0x01, 0x0202, conn->config.interface); // Assert RTS
    }

    ESP_LOGI(TAG, "FTDI successfully configured (baud=%d, autoboot=%d)", baud, conn->config.autoboot);
  }

  // ── CDC-ACM SET_LINE_CODING ───────────────────────────────
  void cdc_set_line_coding_(DeviceConnection *conn) {
    uint8_t data[7];
    uint32_t baud = conn->config.baud_rate;
    memcpy(data, &baud, 4);
    data[4] = 0;
    data[5] = 0;
    data[6] = 8;
    
    ctrl_transfer_sync_out_(conn->dev_hdl,
          USB_BM_REQUEST_TYPE_DIR_OUT | USB_BM_REQUEST_TYPE_TYPE_CLASS | USB_BM_REQUEST_TYPE_RECIP_INTERFACE,
          CDC_SET_LINE_CODING, 0, conn->config.interface, 7, data);
          
    ESP_LOGD(TAG, "CDC-ACM line coding set: %d baud, 8N1", baud);
  }

  // ── Close USB device ──────────────────────────────────────
  void close_device_(usb_device_handle_t dev) {
    for (auto *conn : connections_) {
      if (conn->dev_hdl == dev) {
        conn->connected.store(false);
        xSemaphoreTake(conn->usb_mutex, portMAX_DELAY);
        usb_host_interface_release(client_hdl_, dev, conn->claimed_intf);
        usb_host_device_close(client_hdl_, dev);
        conn->dev_hdl = nullptr;
        xSemaphoreGive(conn->usb_mutex);
        conn->chip_type = ChipType::UNKNOWN;
        ESP_LOGI(TAG, "USB mapping on port %d cleanly closed", conn->config.port);
        // Break is safe assuming a device handle only maps to one slot
        break;
      }
    }
  }

  // ── Bulk read task: USB → TCP ─────────────────────────────
  static void bulk_read_task_entry_(void *arg) {
    auto *conn = static_cast<DeviceConnection *>(arg);
    conn->parent->bulk_read_task_(conn);
  }

  void bulk_read_task_(DeviceConnection *conn) {
    usb_transfer_t *xfer;
    esp_err_t err = usb_host_transfer_alloc(conn->bulk_in_mps, 0, &xfer);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "Failed to alloc bulk IN transfer (Port %d)", conn->config.port);
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

    while (conn->connected.load()) {
      err = usb_host_transfer_submit(xfer);
      if (err != ESP_OK) {
        if (conn->connected.load()) {
          ESP_LOGW(TAG, "Bulk IN submit failed on port %d: %s", conn->config.port, esp_err_to_name(err));
        }
        break;
      }
      xSemaphoreTake(conn->bulk_in_sem, portMAX_DELAY);
    }

    usb_host_transfer_free(xfer);
    if (conn->bulk_in_sem) {
        vSemaphoreDelete(conn->bulk_in_sem);
        conn->bulk_in_sem = nullptr;
    }
    
    ESP_LOGI(TAG, "Bulk read task ended (Port %d)", conn->config.port);
    vTaskDelete(nullptr);
  }

  static void bulk_in_cb_(usb_transfer_t *xfer) {
    auto *conn = static_cast<DeviceConnection *>(xfer->context);
    
    if (xfer->status == USB_TRANSFER_STATUS_COMPLETED && xfer->actual_num_bytes > 0) {
      const uint8_t *data = xfer->data_buffer;
      size_t len = xfer->actual_num_bytes;

      if (conn->chip_type == ChipType::FTDI) {
        if (len > 2) {
          data += 2;
          len -= 2;
          int fd = conn->tcp_client_fd.load();
          if (fd >= 0) {
            lwip_send(fd, data, len, MSG_DONTWAIT);
          }
        }
      } else {
        int fd = conn->tcp_client_fd.load();
        if (fd >= 0) {
          lwip_send(fd, data, len, MSG_DONTWAIT);
        }
      }
    }

    if (conn->bulk_in_sem) {
      xSemaphoreGiveFromISR(conn->bulk_in_sem, nullptr);
    }
  }

  // ── USB monitor task ──────────────────────────────────────
  static void usb_task_entry_(void *arg) {
    static_cast<UsbBridgeComponent *>(arg)->usb_task_();
  }

  void usb_task_() {
    ESP_LOGI(TAG, "USB client mon task started, waiting for events...");

    while (true) {
      esp_err_t err = usb_host_client_handle_events(client_hdl_, pdMS_TO_TICKS(1000));
      if (err != ESP_OK && err != ESP_ERR_TIMEOUT) {
        // Suppress timeout logs to avoid console spam
        vTaskDelay(pdMS_TO_TICKS(100));
      }
    }
  }

  // ── TCP → USB write ───────────────────────────────────────
  void usb_write_(DeviceConnection *conn, const uint8_t *data, size_t len) {
    xSemaphoreTake(conn->usb_mutex, portMAX_DELAY);
    if (!conn->dev_hdl || !conn->connected.load()) {
      xSemaphoreGive(conn->usb_mutex);
      return;
    }

    usb_transfer_t *xfer;
    esp_err_t err = usb_host_transfer_alloc(len, 0, &xfer);
    if (err != ESP_OK) {
      xSemaphoreGive(conn->usb_mutex);
      return;
    }

    memcpy(xfer->data_buffer, data, len);
    xfer->device_handle = conn->dev_hdl;
    xfer->bEndpointAddress = conn->bulk_out_ep;
    xfer->callback = bulk_out_cb_;
    xfer->context = nullptr;
    xfer->num_bytes = len;
    xfer->timeout_ms = 1000;

    err = usb_host_transfer_submit(xfer);
    if (err != ESP_OK) {
      ESP_LOGW(TAG, "Bulk OUT submit failed on port %d: %s", conn->config.port, esp_err_to_name(err));
      usb_host_transfer_free(xfer);
    }
    xSemaphoreGive(conn->usb_mutex);
  }

  static void bulk_out_cb_(usb_transfer_t *xfer) {
    if (xfer->status != USB_TRANSFER_STATUS_COMPLETED) {
      ESP_LOGW(TAG, "Bulk OUT failed: status=%d", xfer->status);
    }
    usb_host_transfer_free(xfer);
  }

  // ── TCP server task ───────────────────────────────────────
  static void tcp_task_entry_(void *arg) {
    auto *conn = static_cast<DeviceConnection *>(arg);
    conn->parent->tcp_task_for_conn_(conn);
  }

  void tcp_task_for_conn_(DeviceConnection *conn) {
    vTaskDelay(pdMS_TO_TICKS(5000));

    while (true) {
      int server_fd = lwip_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
      if (server_fd < 0) {
        ESP_LOGE(TAG, "socket() failed on port %d", conn->config.port);
        vTaskDelay(pdMS_TO_TICKS(5000));
        continue;
      }

      int opt = 1;
      lwip_setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

      struct sockaddr_in addr = {};
      addr.sin_family = AF_INET;
      addr.sin_addr.s_addr = INADDR_ANY;
      addr.sin_port = htons(conn->config.port);

      if (lwip_bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "bind() port %d failed", conn->config.port);
        lwip_close(server_fd);
        vTaskDelay(pdMS_TO_TICKS(5000));
        continue;
      }

      if (lwip_listen(server_fd, 1) < 0) {
        ESP_LOGE(TAG, "listen() failed for port %d", conn->config.port);
        lwip_close(server_fd);
        vTaskDelay(pdMS_TO_TICKS(5000));
        continue;
      }

      ESP_LOGI(TAG, "TCP server listening on port %d", conn->config.port);

      while (true) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = lwip_accept(server_fd,
                                    (struct sockaddr *)&client_addr, &client_len);
        if (client_fd < 0) {
          ESP_LOGE(TAG, "accept() failed on port %d", conn->config.port);
          break;
        }

        char addr_str[INET_ADDRSTRLEN];
        inet_ntoa_r(client_addr.sin_addr, addr_str, sizeof(addr_str));
        ESP_LOGI(TAG, "Client connected on %d: %s", conn->config.port, addr_str);

        opt = 1;
        lwip_setsockopt(client_fd, SOL_SOCKET, SO_KEEPALIVE, &opt, sizeof(opt));
        conn->tcp_client_fd.store(client_fd);

        uint8_t buf[256];
        while (true) {
          int len = lwip_recv(client_fd, buf, sizeof(buf), 0);
          if (len <= 0) {
            break;
          }
          if (conn->connected.load()) {
            conn->parent->usb_write_(conn, buf, len);
          }
        }

        ESP_LOGI(TAG, "Client disconnected on %d", conn->config.port);
        conn->tcp_client_fd.store(-1);
        lwip_close(client_fd);
      }

      lwip_close(server_fd);
    }
  }
};

UsbBridgeComponent *UsbBridgeComponent::instance_ = nullptr;

// ── HTTP Handler Implementations (need UsbBridgeComponent) ──

static esp_err_t handle_get_config_(httpd_req_t *req) {
  auto *comp = web_ctx_.component;
  if (!comp) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No component");
    return ESP_FAIL;
  }

  auto &conns = comp->get_connections();
  // Build JSON array
  char buf[2048];
  char *p = buf;
  const char *end = buf + sizeof(buf) - 2;
  *p++ = '[';

  for (size_t i = 0; i < conns.size() && p < end - 200; i++) {
    auto &cfg = conns[i]->config;
    if (i > 0) *p++ = ',';
    *p++ = '{';
    json_append_str(p, end, "name", cfg.name);

    // VID/PID as hex strings
    char hex[8];
    snprintf(hex, sizeof(hex), "%04X", cfg.vid);
    json_append_int(p, end, "vid", cfg.vid);
    snprintf(hex, sizeof(hex), "%04X", cfg.pid);
    json_append_int(p, end, "pid", cfg.pid);
    json_append_int(p, end, "port", cfg.port);
    json_append_int(p, end, "baud_rate", cfg.baud_rate);
    json_append_int(p, end, "interface", cfg.interface);
    json_append_bool(p, end, "autoboot", cfg.autoboot, false);
    *p++ = '}';
  }
  *p++ = ']';
  *p = 0;

  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, buf, p - buf);
  return ESP_OK;
}

static esp_err_t handle_get_status_(httpd_req_t *req) {
  auto *comp = web_ctx_.component;
  if (!comp) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No component");
    return ESP_FAIL;
  }

  auto &conns = comp->get_connections();
  char buf[512];
  char *p = buf;
  const char *end = buf + sizeof(buf) - 2;
  *p++ = '[';

  for (size_t i = 0; i < conns.size() && p < end - 50; i++) {
    if (i > 0) *p++ = ',';
    *p++ = '{';
    json_append_bool(p, end, "connected", conns[i]->connected.load());
    json_append_int(p, end, "port", conns[i]->config.port, false);
    *p++ = '}';
  }
  *p++ = ']';
  *p = 0;

  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, buf, p - buf);
  return ESP_OK;
}

static esp_err_t handle_post_config_(httpd_req_t *req) {
  auto *comp = web_ctx_.component;
  if (!comp) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No component");
    return ESP_FAIL;
  }

  // Read POST body
  int total_len = req->content_len;
  if (total_len > 4096) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Too large");
    return ESP_FAIL;
  }

  char *body = (char *)malloc(total_len + 1);
  if (!body) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
    return ESP_FAIL;
  }

  int received = 0;
  while (received < total_len) {
    int ret = httpd_req_recv(req, body + received, total_len - received);
    if (ret <= 0) {
      free(body);
      httpd_resp_send_err(req, HTTPD_408_REQ_TIMEOUT, "Timeout");
      return ESP_FAIL;
    }
    received += ret;
  }
  body[total_len] = 0;

  // Parse JSON array of device configs
  // Simple parser: find each {...} object
  std::vector<StoredDeviceConfig> devs;
  const char *ptr = body;

  while (*ptr) {
    const char *obj_start = strchr(ptr, '{');
    if (!obj_start) break;
    const char *obj_end = strchr(obj_start, '}');
    if (!obj_end) break;

    // Extract object substring
    char obj[512];
    size_t obj_len = obj_end - obj_start + 1;
    if (obj_len >= sizeof(obj)) obj_len = sizeof(obj) - 1;
    memcpy(obj, obj_start, obj_len);
    obj[obj_len] = 0;

    StoredDeviceConfig cfg = {};
    json_get_str(obj, "name", cfg.name, sizeof(cfg.name));
    cfg.vid = json_get_int(obj, "vid", 0);
    cfg.pid = json_get_int(obj, "pid", 0);
    cfg.port = json_get_int(obj, "port", 8880);
    cfg.baud_rate = json_get_int(obj, "baud_rate", 115200);
    cfg.interface = json_get_int(obj, "interface", 0);
    cfg.autoboot = json_get_bool(obj, "autoboot", false) ? 1 : 0;

    if (cfg.vid > 0 && cfg.pid > 0 && cfg.port > 0) {
      devs.push_back(cfg);
      ESP_LOGI(WEB_TAG, "Parsed device: %s VID=%04X PID=%04X port=%d",
               cfg.name, cfg.vid, cfg.pid, cfg.port);
    }

    ptr = obj_end + 1;
  }

  free(body);

  if (devs.empty()) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No valid devices");
    return ESP_FAIL;
  }

  // Save to NVS
  if (!nvs_save_devices(devs)) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "NVS save failed");
    return ESP_FAIL;
  }

  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr(req, "{\"message\":\"Config saved! Rebooting...\",\"ok\":true}");

  // Reboot after response is sent
  xTaskCreate([](void *) {
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
  }, "reboot", 2048, nullptr, 1, nullptr);

  return ESP_OK;
}

}  // namespace usb_bridge
}  // namespace esphome
