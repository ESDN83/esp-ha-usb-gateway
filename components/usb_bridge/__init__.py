import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_BAUD_RATE
from esphome.components.esp32 import add_idf_sdkconfig_option

DEPENDENCIES = ["network"]
CODEOWNERS = []

CONF_TCP_PORT = "tcp_port"

usb_bridge_ns = cg.esphome_ns.namespace("usb_bridge")
UsbBridgeComponent = usb_bridge_ns.class_("UsbBridgeComponent", cg.Component)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(UsbBridgeComponent),
        cv.Optional(CONF_TCP_PORT, default=8880): cv.port,
        cv.Optional(CONF_BAUD_RATE, default=57600): cv.positive_int,
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_tcp_port(config[CONF_TCP_PORT]))
    cg.add(var.set_baud_rate(config[CONF_BAUD_RATE]))

    # No external IDF components needed — uses only the built-in USB Host API
    add_idf_sdkconfig_option("CONFIG_USB_OTG_SUPPORTED", True)
    add_idf_sdkconfig_option("CONFIG_USB_HOST_CONTROL_TRANSFER_MAX_SIZE", 1024)

    # Hub support (ESP-IDF 5.2+, ESPHome 2026.1 uses IDF 5.5.2)
    add_idf_sdkconfig_option("CONFIG_USB_HOST_HUBS_SUPPORTED", True)
    add_idf_sdkconfig_option("CONFIG_USB_HOST_HUB_MULTI_LEVEL", True)
    add_idf_sdkconfig_option("CONFIG_USB_HOST_EXT_HUB_MAX_PORTS", 4)

    # Increase USB timing for slow-starting hubs (ESP-IDF #10086, #12412)
    add_idf_sdkconfig_option("CONFIG_USB_HOST_DEBOUNCE_DELAY_MS", 300)
    add_idf_sdkconfig_option("CONFIG_USB_HOST_RESET_HOLD_MS", 50)
    add_idf_sdkconfig_option("CONFIG_USB_HOST_RESET_RECOVERY_MS", 50)
    add_idf_sdkconfig_option("CONFIG_USB_HOST_SET_ADDR_RECOVERY_MS", 20)
