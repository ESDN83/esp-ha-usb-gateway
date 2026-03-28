#pragma once

#include "esphome/core/component.h"
#include "esphome/core/log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "usb/usb_host.h"
#include "usb/cdc_acm_host.h"
#include <array>  // required before vcp_ftdi.hpp (missing include in v1.0.0)
#include "usb/vcp_ftdi.hpp"
#include "usb/vcp.hpp"

// lwip defines socket/bind/etc. as macros — include then undef to use POSIX API
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "esp_netif.h"

#include <atomic>
#include <cstring>
#include <memory>

namespace esphome {
namespace usb_bridge {

static const char *const TAG = "usb_bridge";

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

    // Install USB Host Library
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

    // USB Host library event handler task
    xTaskCreatePinnedToCore(usb_host_lib_task_, "usb_lib", 4096,
                            nullptr, 10, nullptr, 0);

    // Install CDC-ACM host driver
    const cdc_acm_host_driver_config_t acm_config = {
        .driver_task_stack_size = 4096,
        .driver_task_priority = 10,
        .xCoreID = 0,
        .new_dev_cb = nullptr,
    };
    err = cdc_acm_host_install(&acm_config);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "CDC-ACM driver install failed: %s", esp_err_to_name(err));
      this->mark_failed();
      return;
    }

    // Register FTDI VCP driver (supports FTDI FT232 based USB serial devices)
    esp_usb::VCP::register_driver<esp_usb::FT23x>();

    // Start worker tasks
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
  std::unique_ptr<CdcAcmDevice> vcp_device_;
  std::atomic<bool> usb_connected_{false};
  std::atomic<int> tcp_client_fd_{-1};
  SemaphoreHandle_t usb_mutex_ = xSemaphoreCreateMutex();

  // ── USB Host Library event handler ──────────────────────────
  static void usb_host_lib_task_(void *arg) {
    while (true) {
      uint32_t event_flags;
      usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
      if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
        usb_host_device_free_all();
      }
    }
  }

  // ── USB RX callback → forward to TCP client ─────────────────
  static void handle_rx_(uint8_t *data, size_t data_len, void *arg) {
    if (!instance_ || data_len == 0) return;

    int fd = instance_->tcp_client_fd_.load();
    if (fd >= 0) {
      lwip_send(fd, data, data_len, MSG_DONTWAIT);
    }
  }

  // ── USB device event callback ──────────────────────────────
  static void handle_event_(const cdc_acm_host_dev_event_data_t *event,
                            void *user_ctx) {
    if (!instance_) return;
    switch (event->type) {
      case CDC_ACM_HOST_ERROR:
        ESP_LOGE(TAG, "USB device error");
        break;
      case CDC_ACM_HOST_DEVICE_DISCONNECTED:
        ESP_LOGW(TAG, "USB device disconnected");
        instance_->usb_connected_.store(false);
        break;
      default:
        break;
    }
  }

  // ── USB monitor task: detect and open FTDI device ──────────
  static void usb_task_entry_(void *arg) {
    static_cast<UsbBridgeComponent *>(arg)->usb_task_();
  }

  void usb_task_() {
    while (true) {
      if (!usb_connected_.load()) {
        // Clean up previous device
        xSemaphoreTake(usb_mutex_, portMAX_DELAY);
        vcp_device_.reset();
        xSemaphoreGive(usb_mutex_);

        ESP_LOGI(TAG, "Scanning for FTDI USB serial device...");

        const cdc_acm_host_device_config_t dev_config = {
            .connection_timeout_ms = 5000,
            .out_buffer_size = 512,
            .event_cb = handle_event_,
            .data_cb = handle_rx_,
            .user_arg = nullptr,
        };

        auto vcp = std::unique_ptr<CdcAcmDevice>(esp_usb::VCP::open(&dev_config));
        if (vcp) {
          cdc_acm_line_coding_t line_coding = {
              .dwDTERate = static_cast<uint32_t>(baud_rate_),
              .bCharFormat = 0,  // 1 stop bit
              .bParityType = 0,  // no parity
              .bDataBits = 8,
          };
          esp_err_t err = vcp->line_coding_set(&line_coding);
          if (err != ESP_OK) {
            ESP_LOGW(TAG, "line_coding_set: %s (non-fatal)", esp_err_to_name(err));
          }

          xSemaphoreTake(usb_mutex_, portMAX_DELAY);
          vcp_device_ = std::move(vcp);
          xSemaphoreGive(usb_mutex_);

          usb_connected_.store(true);
          ESP_LOGI(TAG, "USB serial device connected (%d baud)", baud_rate_);
        } else {
          ESP_LOGD(TAG, "No FTDI device found, retry in 5s");
        }
      }
      vTaskDelay(pdMS_TO_TICKS(5000));
    }
  }

  // ── TCP server task: accept clients, bridge TCP↔USB ────────
  static void tcp_task_entry_(void *arg) {
    static_cast<UsbBridgeComponent *>(arg)->tcp_task_();
  }

  void tcp_task_() {
    // Wait for network stack
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

      // Accept loop – one client at a time
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

        // Enable TCP keepalive
        opt = 1;
        lwip_setsockopt(client_fd, SOL_SOCKET, SO_KEEPALIVE, &opt, sizeof(opt));

        tcp_client_fd_.store(client_fd);

        // Forward TCP → USB (USB → TCP happens in handle_rx_ callback)
        uint8_t buf[256];
        while (true) {
          int len = lwip_recv(client_fd, buf, sizeof(buf), 0);
          if (len <= 0) {
            if (len < 0) {
              ESP_LOGW(TAG, "recv() error: errno %d", errno);
            }
            break;
          }

          if (usb_connected_.load()) {
            xSemaphoreTake(usb_mutex_, portMAX_DELAY);
            if (vcp_device_) {
              esp_err_t err = vcp_device_->tx_blocking(buf, len, 1000);
              if (err != ESP_OK) {
                ESP_LOGW(TAG, "USB TX error: %s", esp_err_to_name(err));
              }
            }
            xSemaphoreGive(usb_mutex_);
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
