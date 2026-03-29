#pragma once

#include "esphome/core/component.h"
#include "esphome/core/log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "usb/usb_host.h"

#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "esp_netif.h"

#include <atomic>
#include <cstring>

namespace esphome {
namespace usb_bridge {

static const char *const TAG = "usb_bridge";

// Known USB serial chip vendors
static constexpr uint16_t FTDI_VID = 0x0403;

// FTDI vendor requests
static constexpr uint8_t FTDI_REQ_RESET = 0x00;
static constexpr uint8_t FTDI_REQ_SET_FLOW_CTRL = 0x02;
static constexpr uint8_t FTDI_REQ_SET_BAUDRATE = 0x03;
static constexpr uint8_t FTDI_REQ_SET_DATA = 0x04;

// CDC-ACM class requests
static constexpr uint8_t CDC_SET_LINE_CODING = 0x20;

// USB Hub class requests
static constexpr uint8_t HUB_REQ_GET_DESCRIPTOR = 0x06;
static constexpr uint8_t HUB_REQ_SET_FEATURE = 0x03;
static constexpr uint8_t HUB_REQ_GET_STATUS = 0x00;
static constexpr uint8_t HUB_REQ_CLEAR_FEATURE = 0x01;

// Hub port features
static constexpr uint16_t HUB_PORT_POWER = 8;
static constexpr uint16_t HUB_PORT_RESET = 4;
static constexpr uint16_t HUB_C_PORT_CONNECTION = 16;
static constexpr uint16_t HUB_C_PORT_RESET = 20;

// Hub port status bits
static constexpr uint16_t HUB_PORT_STATUS_CONNECTION = (1 << 0);
static constexpr uint16_t HUB_PORT_STATUS_ENABLE = (1 << 1);
static constexpr uint16_t HUB_PORT_STATUS_RESET = (1 << 4);
static constexpr uint16_t HUB_PORT_STATUS_POWER = (1 << 8);

// USB class codes
#ifndef USB_CLASS_CDC
#define USB_CLASS_CDC 0x02
#endif

enum class ChipType : uint8_t {
  UNKNOWN = 0,
  FTDI,
  CDC_ACM,
  GENERIC,
};

class UsbBridgeComponent : public Component {
 public:
  void set_tcp_port(int port) { this->tcp_port_ = port; }
  void set_baud_rate(int baud) { this->baud_rate_ = baud; }

  float get_setup_priority() const override {
    return setup_priority::AFTER_WIFI;
  }

  void setup() override {
    ESP_LOGI(TAG, "USB TCP Bridge starting (port=%d, baud=%d)",
             tcp_port_, baud_rate_);

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

    xTaskCreatePinnedToCore(usb_lib_task_, "usb_lib", 4096,
                            nullptr, 10, nullptr, 0);
    xTaskCreatePinnedToCore(usb_task_entry_, "usb_mon", 8192,
                            this, 5, nullptr, 1);
    xTaskCreatePinnedToCore(tcp_task_entry_, "tcp_srv", 8192,
                            this, 5, nullptr, 1);

    ESP_LOGI(TAG, "USB TCP Bridge initialized");
  }

  void loop() override {}

 private:
  int tcp_port_{8880};
  int baud_rate_{57600};

  static UsbBridgeComponent *instance_;

  usb_host_client_handle_t client_hdl_{nullptr};
  usb_device_handle_t dev_hdl_{nullptr};
  usb_device_handle_t hub_hdl_{nullptr};
  uint8_t bulk_in_ep_{0};
  uint8_t bulk_out_ep_{0};
  uint16_t bulk_in_mps_{64};
  uint8_t claimed_intf_{0};
  ChipType chip_type_{ChipType::UNKNOWN};

  // Synchronous control transfer support
  SemaphoreHandle_t ctrl_xfer_done_{nullptr};
  usb_transfer_status_t ctrl_xfer_status_{};
  uint16_t ctrl_xfer_actual_{0};

  std::atomic<bool> usb_connected_{false};
  std::atomic<int> tcp_client_fd_{-1};
  SemaphoreHandle_t usb_mutex_ = xSemaphoreCreateMutex();

  // ── USB Host Library daemon ───────────────────────────────
  static void usb_lib_task_(void *arg) {
    while (true) {
      uint32_t event_flags;
      usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
      if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
        usb_host_device_free_all();
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
        self->close_device_();
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

    // Reset semaphore
    xSemaphoreTake(ctrl_xfer_done_, 0);

    err = usb_host_transfer_submit_control(client_hdl_, xfer);
    if (err != ESP_OK) {
      usb_host_transfer_free(xfer);
      return err;
    }

    // Wait for completion
    if (xSemaphoreTake(ctrl_xfer_done_, pdMS_TO_TICKS(3000)) != pdTRUE) {
      usb_host_transfer_free(xfer);
      return ESP_ERR_TIMEOUT;
    }

    if (ctrl_xfer_status_ != USB_TRANSFER_STATUS_COMPLETED) {
      usb_host_transfer_free(xfer);
      return ESP_FAIL;
    }

    // Copy received data if this was an IN transfer
    if ((bmRequestType & USB_BM_REQUEST_TYPE_DIR_IN) && data_out && wLength > 0) {
      size_t copy_len = ctrl_xfer_actual_ > sizeof(usb_setup_packet_t)
                        ? ctrl_xfer_actual_ - sizeof(usb_setup_packet_t) : 0;
      if (copy_len > wLength) copy_len = wLength;
      memcpy(data_out, xfer->data_buffer + sizeof(usb_setup_packet_t), copy_len);
    }

    usb_host_transfer_free(xfer);
    return ESP_OK;
  }

  // ── Hub management ────────────────────────────────────────
  bool handle_hub_(usb_device_handle_t dev) {
    // Get hub descriptor to find number of ports
    uint8_t hub_desc[16] = {};
    esp_err_t err = ctrl_transfer_sync_(
        dev,
        USB_BM_REQUEST_TYPE_DIR_IN | USB_BM_REQUEST_TYPE_TYPE_CLASS |
            USB_BM_REQUEST_TYPE_RECIP_DEVICE,
        HUB_REQ_GET_DESCRIPTOR,
        0x2900,  // Hub Descriptor type
        0, 8, hub_desc);

    uint8_t num_ports = 0;
    if (err == ESP_OK) {
      num_ports = hub_desc[2];
      ESP_LOGI(TAG, "USB Hub has %d ports", num_ports);
    } else {
      ESP_LOGW(TAG, "Get hub descriptor failed: %s, assuming 4 ports",
               esp_err_to_name(err));
      num_ports = 4;
    }

    if (num_ports > 8) num_ports = 8;

    // Power on each port
    for (int port = 1; port <= num_ports; port++) {
      err = ctrl_transfer_sync_(
          dev,
          USB_BM_REQUEST_TYPE_DIR_OUT | USB_BM_REQUEST_TYPE_TYPE_CLASS |
              USB_BM_REQUEST_TYPE_RECIP_OTHER,
          HUB_REQ_SET_FEATURE,
          HUB_PORT_POWER,
          port, 0);
      if (err == ESP_OK) {
        ESP_LOGD(TAG, "Hub port %d powered on", port);
      } else {
        ESP_LOGW(TAG, "Hub port %d power failed: %s", port, esp_err_to_name(err));
      }
    }

    // Wait for ports to power up
    vTaskDelay(pdMS_TO_TICKS(200));

    // Check each port for connected devices and reset them
    for (int port = 1; port <= num_ports; port++) {
      uint8_t status_buf[4] = {};
      err = ctrl_transfer_sync_(
          dev,
          USB_BM_REQUEST_TYPE_DIR_IN | USB_BM_REQUEST_TYPE_TYPE_CLASS |
              USB_BM_REQUEST_TYPE_RECIP_OTHER,
          HUB_REQ_GET_STATUS,
          0, port, 4, status_buf);

      if (err != ESP_OK) {
        ESP_LOGD(TAG, "Hub port %d get_status failed", port);
        continue;
      }

      uint16_t port_status = status_buf[0] | (status_buf[1] << 8);
      ESP_LOGD(TAG, "Hub port %d status: 0x%04X", port, port_status);

      if (!(port_status & HUB_PORT_STATUS_CONNECTION)) {
        ESP_LOGD(TAG, "Hub port %d: no device", port);
        continue;
      }

      ESP_LOGI(TAG, "Hub port %d: device connected, resetting...", port);

      // Clear connection change
      ctrl_transfer_sync_(
          dev,
          USB_BM_REQUEST_TYPE_DIR_OUT | USB_BM_REQUEST_TYPE_TYPE_CLASS |
              USB_BM_REQUEST_TYPE_RECIP_OTHER,
          HUB_REQ_CLEAR_FEATURE,
          HUB_C_PORT_CONNECTION,
          port, 0);

      // Reset the port
      err = ctrl_transfer_sync_(
          dev,
          USB_BM_REQUEST_TYPE_DIR_OUT | USB_BM_REQUEST_TYPE_TYPE_CLASS |
              USB_BM_REQUEST_TYPE_RECIP_OTHER,
          HUB_REQ_SET_FEATURE,
          HUB_PORT_RESET,
          port, 0);

      if (err != ESP_OK) {
        ESP_LOGW(TAG, "Hub port %d reset failed: %s", port, esp_err_to_name(err));
        continue;
      }

      // Wait for reset to complete
      vTaskDelay(pdMS_TO_TICKS(100));

      // Clear reset change
      ctrl_transfer_sync_(
          dev,
          USB_BM_REQUEST_TYPE_DIR_OUT | USB_BM_REQUEST_TYPE_TYPE_CLASS |
              USB_BM_REQUEST_TYPE_RECIP_OTHER,
          HUB_REQ_CLEAR_FEATURE,
          HUB_C_PORT_RESET,
          port, 0);

      // Check port is now enabled
      err = ctrl_transfer_sync_(
          dev,
          USB_BM_REQUEST_TYPE_DIR_IN | USB_BM_REQUEST_TYPE_TYPE_CLASS |
              USB_BM_REQUEST_TYPE_RECIP_OTHER,
          HUB_REQ_GET_STATUS,
          0, port, 4, status_buf);

      if (err == ESP_OK) {
        port_status = status_buf[0] | (status_buf[1] << 8);
        ESP_LOGI(TAG, "Hub port %d after reset: status=0x%04X %s%s",
                 port, port_status,
                 (port_status & HUB_PORT_STATUS_CONNECTION) ? "CONN " : "",
                 (port_status & HUB_PORT_STATUS_ENABLE) ? "ENABLE" : "");
      }
    }

    return true;
  }

  // ── Detect chip type from device descriptor ───────────────
  ChipType detect_chip_type_(const usb_device_desc_t *desc,
                             const usb_config_desc_t *config_desc) {
    if (desc->idVendor == FTDI_VID) {
      ESP_LOGI(TAG, "Detected FTDI chip (PID=%04X)", desc->idProduct);
      return ChipType::FTDI;
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
      ESP_LOGE(TAG, "get_device_desc failed: %s", esp_err_to_name(err));
      usb_host_device_close(client_hdl_, dev);
      return;
    }

    ESP_LOGI(TAG, "USB device: VID=%04X PID=%04X Class=%02X",
             desc->idVendor, desc->idProduct, desc->bDeviceClass);

    // Handle USB hubs: power on ports and reset to enumerate downstream devices
    if (desc->bDeviceClass == USB_CLASS_HUB) {
      ESP_LOGI(TAG, "Found USB hub, managing ports...");
      // Claim hub interface
      err = usb_host_interface_claim(client_hdl_, dev, 0, 0);
      if (err != ESP_OK) {
        ESP_LOGE(TAG, "Hub interface claim failed: %s", esp_err_to_name(err));
        usb_host_device_close(client_hdl_, dev);
        return;
      }
      hub_hdl_ = dev;
      handle_hub_(dev);
      // Keep hub open — don't close it, downstream devices need it
      return;
    }

    // For non-hub devices: try to use as serial bridge
    if (usb_connected_.load()) {
      ESP_LOGD(TAG, "Already connected to a device, skipping");
      usb_host_device_close(client_hdl_, dev);
      return;
    }

    const usb_config_desc_t *config_desc;
    err = usb_host_get_active_config_descriptor(dev, &config_desc);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "get_config_desc failed: %s", esp_err_to_name(err));
      usb_host_device_close(client_hdl_, dev);
      return;
    }

    chip_type_ = detect_chip_type_(desc, config_desc);

    if (!find_bulk_endpoints_(config_desc)) {
      ESP_LOGE(TAG, "No bulk IN/OUT endpoints found");
      usb_host_device_close(client_hdl_, dev);
      return;
    }

    err = usb_host_interface_claim(client_hdl_, dev, claimed_intf_, 0);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "interface_claim(%d) failed: %s", claimed_intf_,
               esp_err_to_name(err));
      usb_host_device_close(client_hdl_, dev);
      return;
    }

    xSemaphoreTake(usb_mutex_, portMAX_DELAY);
    dev_hdl_ = dev;
    xSemaphoreGive(usb_mutex_);

    if (chip_type_ == ChipType::FTDI) {
      ftdi_init_(dev);
    } else if (chip_type_ == ChipType::CDC_ACM) {
      cdc_set_line_coding_(dev);
    }

    usb_connected_.store(true);

    const char *type_str = chip_type_ == ChipType::FTDI ? "FTDI" :
                           chip_type_ == ChipType::CDC_ACM ? "CDC-ACM" : "Generic";
    ESP_LOGI(TAG, "USB device ready: %s (VID=%04X PID=%04X, %d baud, EP IN=0x%02X OUT=0x%02X)",
             type_str, desc->idVendor, desc->idProduct, baud_rate_,
             bulk_in_ep_, bulk_out_ep_);

    xTaskCreatePinnedToCore(bulk_read_task_entry_, "usb_rx", 4096,
                            this, 6, nullptr, 1);
  }

  // ── Find bulk IN and OUT endpoints ────────────────────────
  bool find_bulk_endpoints_(const usb_config_desc_t *config_desc) {
    bulk_in_ep_ = 0;
    bulk_out_ep_ = 0;

    for (int i = 0; i < config_desc->bNumInterfaces; i++) {
      int offset = 0;
      const usb_intf_desc_t *intf = usb_parse_interface_descriptor(
          config_desc, i, 0, &offset);
      if (!intf) continue;
      if (intf->bNumEndpoints == 0) continue;
      if (intf->bInterfaceClass == USB_CLASS_CDC && chip_type_ == ChipType::CDC_ACM) continue;

      uint8_t in_ep = 0, out_ep = 0;
      uint16_t in_mps = 64;

      for (int j = 0; j < intf->bNumEndpoints; j++) {
        const usb_ep_desc_t *ep = usb_parse_endpoint_descriptor_by_index(
            intf, j, config_desc->wTotalLength, &offset);
        if (!ep) continue;
        if ((ep->bmAttributes & USB_BM_ATTRIBUTES_XFERTYPE_MASK) == USB_BM_ATTRIBUTES_XFER_BULK) {
          if (ep->bEndpointAddress & 0x80) {
            in_ep = ep->bEndpointAddress;
            in_mps = ep->wMaxPacketSize;
          } else {
            out_ep = ep->bEndpointAddress;
          }
        }
      }

      if (in_ep && out_ep) {
        bulk_in_ep_ = in_ep;
        bulk_out_ep_ = out_ep;
        bulk_in_mps_ = in_mps;
        claimed_intf_ = intf->bInterfaceNumber;
        ESP_LOGD(TAG, "Found bulk endpoints on interface %d: IN=0x%02X (MPS=%d) OUT=0x%02X",
                 claimed_intf_, in_ep, in_mps, out_ep);
        return true;
      }
    }
    return false;
  }

  // ── FTDI initialization ───────────────────────────────────
  void ftdi_init_(usb_device_handle_t dev) {
    auto vc = [&](uint8_t req, uint16_t val, uint16_t idx) {
      ctrl_transfer_sync_(dev,
          USB_BM_REQUEST_TYPE_DIR_OUT | USB_BM_REQUEST_TYPE_TYPE_VENDOR |
              USB_BM_REQUEST_TYPE_RECIP_DEVICE,
          req, val, idx, 0);
    };
    vc(FTDI_REQ_RESET, 0, 0);
    vc(FTDI_REQ_RESET, 1, 0);
    vc(FTDI_REQ_RESET, 2, 0);
    vc(FTDI_REQ_SET_BAUDRATE, 3000000 / baud_rate_, 0);
    vc(FTDI_REQ_SET_DATA, 0x0008, 0);
    vc(FTDI_REQ_SET_FLOW_CTRL, 0, 0);
    ESP_LOGD(TAG, "FTDI configured: %d baud, 8N1", baud_rate_);
  }

  // ── CDC-ACM SET_LINE_CODING ───────────────────────────────
  void cdc_set_line_coding_(usb_device_handle_t dev) {
    usb_transfer_t *xfer;
    esp_err_t err = usb_host_transfer_alloc(sizeof(usb_setup_packet_t) + 7, 0, &xfer);
    if (err != ESP_OK) return;

    xfer->device_handle = dev;
    xfer->bEndpointAddress = 0;
    xfer->callback = ctrl_xfer_sync_cb_;
    xfer->context = this;
    xfer->timeout_ms = 1000;

    usb_setup_packet_t *setup = (usb_setup_packet_t *)xfer->data_buffer;
    setup->bmRequestType = USB_BM_REQUEST_TYPE_DIR_OUT |
                           USB_BM_REQUEST_TYPE_TYPE_CLASS |
                           USB_BM_REQUEST_TYPE_RECIP_INTERFACE;
    setup->bRequest = CDC_SET_LINE_CODING;
    setup->wValue = 0;
    setup->wIndex = 0;
    setup->wLength = 7;

    uint8_t *data = xfer->data_buffer + sizeof(usb_setup_packet_t);
    uint32_t baud = static_cast<uint32_t>(baud_rate_);
    memcpy(data, &baud, 4);
    data[4] = 0;
    data[5] = 0;
    data[6] = 8;
    xfer->num_bytes = sizeof(usb_setup_packet_t) + 7;

    xSemaphoreTake(ctrl_xfer_done_, 0);
    err = usb_host_transfer_submit_control(client_hdl_, xfer);
    if (err != ESP_OK) {
      usb_host_transfer_free(xfer);
      return;
    }
    if (xSemaphoreTake(ctrl_xfer_done_, pdMS_TO_TICKS(2000)) == pdTRUE) {
      ESP_LOGD(TAG, "CDC-ACM line coding set: %d baud, 8N1", baud_rate_);
    }
    usb_host_transfer_free(xfer);
  }

  // ── Close USB device ──────────────────────────────────────
  void close_device_() {
    usb_connected_.store(false);
    xSemaphoreTake(usb_mutex_, portMAX_DELAY);
    if (dev_hdl_) {
      usb_host_interface_release(client_hdl_, dev_hdl_, claimed_intf_);
      usb_host_device_close(client_hdl_, dev_hdl_);
      dev_hdl_ = nullptr;
    }
    if (hub_hdl_) {
      usb_host_interface_release(client_hdl_, hub_hdl_, 0);
      usb_host_device_close(client_hdl_, hub_hdl_);
      hub_hdl_ = nullptr;
    }
    xSemaphoreGive(usb_mutex_);
    chip_type_ = ChipType::UNKNOWN;
    ESP_LOGI(TAG, "USB device closed");
  }

  // ── Bulk read task: USB → TCP ─────────────────────────────
  static void bulk_read_task_entry_(void *arg) {
    static_cast<UsbBridgeComponent *>(arg)->bulk_read_task_();
  }

  void bulk_read_task_() {
    usb_transfer_t *xfer;
    esp_err_t err = usb_host_transfer_alloc(bulk_in_mps_, 0, &xfer);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "Failed to alloc bulk IN transfer");
      return;
    }

    xfer->device_handle = dev_hdl_;
    xfer->bEndpointAddress = bulk_in_ep_;
    xfer->callback = bulk_in_cb_;
    xfer->context = this;
    xfer->num_bytes = bulk_in_mps_;
    xfer->timeout_ms = 500;

    while (usb_connected_.load()) {
      err = usb_host_transfer_submit(xfer);
      if (err != ESP_OK) {
        if (usb_connected_.load()) {
          ESP_LOGW(TAG, "Bulk IN submit failed: %s", esp_err_to_name(err));
        }
        break;
      }
      vTaskDelay(pdMS_TO_TICKS(10));
    }

    usb_host_transfer_free(xfer);
    ESP_LOGI(TAG, "Bulk read task ended");
    vTaskDelete(nullptr);
  }

  static void bulk_in_cb_(usb_transfer_t *xfer) {
    auto *self = static_cast<UsbBridgeComponent *>(xfer->context);
    if (xfer->status != USB_TRANSFER_STATUS_COMPLETED || xfer->actual_num_bytes == 0) return;

    const uint8_t *data = xfer->data_buffer;
    size_t len = xfer->actual_num_bytes;

    if (self->chip_type_ == ChipType::FTDI) {
      if (len <= 2) return;
      data += 2;
      len -= 2;
    }

    int fd = self->tcp_client_fd_.load();
    if (fd >= 0 && len > 0) {
      lwip_send(fd, data, len, MSG_DONTWAIT);
    }
  }

  // ── USB monitor task ──────────────────────────────────────
  static void usb_task_entry_(void *arg) {
    static_cast<UsbBridgeComponent *>(arg)->usb_task_();
  }

  void usb_task_() {
    const usb_host_client_config_t client_config = {
        .is_synchronous = false,
        .max_num_event_msg = 5,
        .async = {
            .client_event_callback = client_event_cb_,
            .callback_arg = this,
        },
    };
    esp_err_t err = usb_host_client_register(&client_config, &client_hdl_);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "Client register failed: %s", esp_err_to_name(err));
      vTaskDelete(nullptr);
      return;
    }

    ESP_LOGI(TAG, "USB host client registered, waiting for device...");

    while (true) {
      usb_host_client_handle_events(client_hdl_, pdMS_TO_TICKS(1000));
    }
  }

  // ── TCP → USB write ───────────────────────────────────────
  void usb_write_(const uint8_t *data, size_t len) {
    xSemaphoreTake(usb_mutex_, portMAX_DELAY);
    if (!dev_hdl_ || !usb_connected_.load()) {
      xSemaphoreGive(usb_mutex_);
      return;
    }

    usb_transfer_t *xfer;
    esp_err_t err = usb_host_transfer_alloc(len, 0, &xfer);
    if (err != ESP_OK) {
      xSemaphoreGive(usb_mutex_);
      return;
    }

    memcpy(xfer->data_buffer, data, len);
    xfer->device_handle = dev_hdl_;
    xfer->bEndpointAddress = bulk_out_ep_;
    xfer->callback = bulk_out_cb_;
    xfer->context = nullptr;
    xfer->num_bytes = len;
    xfer->timeout_ms = 1000;

    err = usb_host_transfer_submit(xfer);
    if (err != ESP_OK) {
      ESP_LOGW(TAG, "Bulk OUT submit failed: %s", esp_err_to_name(err));
      usb_host_transfer_free(xfer);
    }
    xSemaphoreGive(usb_mutex_);
  }

  static void bulk_out_cb_(usb_transfer_t *xfer) {
    if (xfer->status != USB_TRANSFER_STATUS_COMPLETED) {
      ESP_LOGW(TAG, "Bulk OUT failed: status=%d", xfer->status);
    }
    usb_host_transfer_free(xfer);
  }

  // ── TCP server task ───────────────────────────────────────
  static void tcp_task_entry_(void *arg) {
    static_cast<UsbBridgeComponent *>(arg)->tcp_task_();
  }

  void tcp_task_() {
    vTaskDelay(pdMS_TO_TICKS(5000));

    while (true) {
      int server_fd = lwip_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
      if (server_fd < 0) {
        ESP_LOGE(TAG, "socket() failed: errno %d", errno);
        vTaskDelay(pdMS_TO_TICKS(5000));
        continue;
      }

      int opt = 1;
      lwip_setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

      struct sockaddr_in addr = {};
      addr.sin_family = AF_INET;
      addr.sin_addr.s_addr = INADDR_ANY;
      addr.sin_port = htons(tcp_port_);

      if (lwip_bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "bind() port %d failed: errno %d", tcp_port_, errno);
        lwip_close(server_fd);
        vTaskDelay(pdMS_TO_TICKS(5000));
        continue;
      }

      if (lwip_listen(server_fd, 1) < 0) {
        ESP_LOGE(TAG, "listen() failed: errno %d", errno);
        lwip_close(server_fd);
        vTaskDelay(pdMS_TO_TICKS(5000));
        continue;
      }

      ESP_LOGI(TAG, "TCP server listening on port %d", tcp_port_);

      while (true) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = lwip_accept(server_fd,
                                    (struct sockaddr *)&client_addr, &client_len);
        if (client_fd < 0) {
          ESP_LOGE(TAG, "accept() failed: errno %d", errno);
          break;
        }

        char addr_str[INET_ADDRSTRLEN];
        inet_ntoa_r(client_addr.sin_addr, addr_str, sizeof(addr_str));
        ESP_LOGI(TAG, "Client connected: %s", addr_str);

        opt = 1;
        lwip_setsockopt(client_fd, SOL_SOCKET, SO_KEEPALIVE, &opt, sizeof(opt));
        tcp_client_fd_.store(client_fd);

        uint8_t buf[256];
        while (true) {
          int len = lwip_recv(client_fd, buf, sizeof(buf), 0);
          if (len <= 0) {
            if (len < 0) ESP_LOGW(TAG, "recv() error: errno %d", errno);
            break;
          }
          if (usb_connected_.load()) {
            usb_write_(buf, len);
          }
        }

        ESP_LOGI(TAG, "Client disconnected: %s", addr_str);
        tcp_client_fd_.store(-1);
        lwip_close(client_fd);
      }

      lwip_close(server_fd);
    }
  }
};

UsbBridgeComponent *UsbBridgeComponent::instance_ = nullptr;

}  // namespace usb_bridge
}  // namespace esphome
