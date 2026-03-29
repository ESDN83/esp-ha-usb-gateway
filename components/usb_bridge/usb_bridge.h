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

// FTDI vendor requests (bmRequestType = 0x40 host-to-device, vendor)
static constexpr uint8_t FTDI_REQ_RESET = 0x00;
static constexpr uint8_t FTDI_REQ_SET_FLOW_CTRL = 0x02;
static constexpr uint8_t FTDI_REQ_SET_BAUDRATE = 0x03;
static constexpr uint8_t FTDI_REQ_SET_DATA = 0x04;

// CDC-ACM class requests
static constexpr uint8_t CDC_SET_LINE_CODING = 0x20;

// USB class codes (use ESP-IDF macros where available, define missing ones)
#ifndef USB_CLASS_CDC
#define USB_CLASS_CDC 0x02
#endif

enum class ChipType : uint8_t {
  UNKNOWN = 0,
  FTDI,      // FT232, FT231 etc. — vendor-specific, 2-byte status prefix
  CDC_ACM,   // Standard CDC-ACM (e.g. Arduino, some CP210x, CH340 in CDC mode)
  GENERIC,   // Has bulk endpoints but unknown protocol
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
    xTaskCreatePinnedToCore(usb_task_entry_, "usb_mon", 4096,
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
  uint8_t bulk_in_ep_{0};
  uint8_t bulk_out_ep_{0};
  uint16_t bulk_in_mps_{64};
  uint8_t claimed_intf_{0};
  ChipType chip_type_{ChipType::UNKNOWN};

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

  // ── Detect chip type from device descriptor ───────────────
  ChipType detect_chip_type_(const usb_device_desc_t *desc,
                             const usb_config_desc_t *config_desc) {
    // FTDI: vendor 0x0403
    if (desc->idVendor == FTDI_VID) {
      ESP_LOGI(TAG, "Detected FTDI chip (PID=%04X)", desc->idProduct);
      return ChipType::FTDI;
    }

    // Check interface class for CDC-ACM
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

  // ── Try to open any USB serial device ─────────────────────
  void try_open_device_(uint8_t dev_addr) {
    if (usb_connected_.load()) {
      ESP_LOGD(TAG, "Already connected to a device, skipping");
      return;
    }

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

    // Skip USB hubs
    if (desc->bDeviceClass == USB_CLASS_HUB) {
      ESP_LOGD(TAG, "Skipping USB hub");
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

    // Detect chip type
    chip_type_ = detect_chip_type_(desc, config_desc);

    // Find bulk endpoints (scan all interfaces)
    if (!find_bulk_endpoints_(config_desc)) {
      ESP_LOGE(TAG, "No bulk IN/OUT endpoints found");
      usb_host_device_close(client_hdl_, dev);
      return;
    }

    // Claim the interface that has the bulk endpoints
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

    // Chip-specific initialization
    if (chip_type_ == ChipType::FTDI) {
      ftdi_init_();
    } else if (chip_type_ == ChipType::CDC_ACM) {
      cdc_set_line_coding_();
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

  // ── Find bulk IN and OUT endpoints (all interfaces) ───────
  bool find_bulk_endpoints_(const usb_config_desc_t *config_desc) {
    bulk_in_ep_ = 0;
    bulk_out_ep_ = 0;

    for (int i = 0; i < config_desc->bNumInterfaces; i++) {
      int offset = 0;
      const usb_intf_desc_t *intf = usb_parse_interface_descriptor(
          config_desc, i, 0, &offset);
      if (!intf) continue;

      // Skip interfaces without endpoints
      if (intf->bNumEndpoints == 0) continue;

      // For CDC-ACM: prefer the data interface (class 0x0A), not the control interface (0x02)
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
  void ftdi_init_() {
    ftdi_control_(FTDI_REQ_RESET, 0, 0);       // Reset
    ftdi_control_(FTDI_REQ_RESET, 1, 0);       // Purge RX
    ftdi_control_(FTDI_REQ_RESET, 2, 0);       // Purge TX
    uint16_t divisor = 3000000 / baud_rate_;
    ftdi_control_(FTDI_REQ_SET_BAUDRATE, divisor, 0);
    ftdi_control_(FTDI_REQ_SET_DATA, 0x0008, 0);   // 8N1
    ftdi_control_(FTDI_REQ_SET_FLOW_CTRL, 0, 0);   // No flow control
    ESP_LOGD(TAG, "FTDI configured: %d baud, 8N1", baud_rate_);
  }

  esp_err_t ftdi_control_(uint8_t request, uint16_t value, uint16_t index) {
    usb_transfer_t *xfer;
    esp_err_t err = usb_host_transfer_alloc(sizeof(usb_setup_packet_t), 0, &xfer);
    if (err != ESP_OK) return err;

    xfer->device_handle = dev_hdl_;
    xfer->bEndpointAddress = 0;
    xfer->callback = control_xfer_cb_;
    xfer->context = this;
    xfer->timeout_ms = 1000;

    usb_setup_packet_t *setup = (usb_setup_packet_t *)xfer->data_buffer;
    setup->bmRequestType = USB_BM_REQUEST_TYPE_DIR_OUT |
                           USB_BM_REQUEST_TYPE_TYPE_VENDOR |
                           USB_BM_REQUEST_TYPE_RECIP_DEVICE;
    setup->bRequest = request;
    setup->wValue = value;
    setup->wIndex = index;
    setup->wLength = 0;
    xfer->num_bytes = sizeof(usb_setup_packet_t);

    err = usb_host_transfer_submit_control(client_hdl_, xfer);
    if (err != ESP_OK) {
      usb_host_transfer_free(xfer);
      return err;
    }
    vTaskDelay(pdMS_TO_TICKS(50));
    return ESP_OK;
  }

  // ── CDC-ACM SET_LINE_CODING ───────────────────────────────
  void cdc_set_line_coding_() {
    // SET_LINE_CODING: 7 bytes payload (baud, stop, parity, data bits)
    usb_transfer_t *xfer;
    esp_err_t err = usb_host_transfer_alloc(sizeof(usb_setup_packet_t) + 7, 0, &xfer);
    if (err != ESP_OK) return;

    xfer->device_handle = dev_hdl_;
    xfer->bEndpointAddress = 0;
    xfer->callback = control_xfer_cb_;
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

    // Line coding data: baud(4), stop(1), parity(1), databits(1)
    uint8_t *data = xfer->data_buffer + sizeof(usb_setup_packet_t);
    uint32_t baud = static_cast<uint32_t>(baud_rate_);
    memcpy(data, &baud, 4);  // dwDTERate (little-endian)
    data[4] = 0;  // bCharFormat: 1 stop bit
    data[5] = 0;  // bParityType: none
    data[6] = 8;  // bDataBits: 8
    xfer->num_bytes = sizeof(usb_setup_packet_t) + 7;

    err = usb_host_transfer_submit_control(client_hdl_, xfer);
    if (err != ESP_OK) {
      usb_host_transfer_free(xfer);
      return;
    }
    vTaskDelay(pdMS_TO_TICKS(50));
    ESP_LOGD(TAG, "CDC-ACM line coding set: %d baud, 8N1", baud_rate_);
  }

  static void control_xfer_cb_(usb_transfer_t *xfer) {
    if (xfer->status != USB_TRANSFER_STATUS_COMPLETED) {
      ESP_LOGW(TAG, "Control transfer status=%d", xfer->status);
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

    // FTDI: strip 2-byte modem/line status prefix
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
