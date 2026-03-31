import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID
from esphome.components.esp32 import add_idf_sdkconfig_option

DEPENDENCIES = ["network"]
CODEOWNERS = []

usb_bridge_ns = cg.esphome_ns.namespace("usb_bridge")
UsbBridgeComponent = usb_bridge_ns.class_("UsbBridgeComponent", cg.Component)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(UsbBridgeComponent),
    }
).extend(cv.COMPONENT_SCHEMA)

async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    # USB host on ESP32-S3
    add_idf_sdkconfig_option("CONFIG_USB_OTG_SUPPORTED", True)
    add_idf_sdkconfig_option("CONFIG_USB_HOST_CONTROL_TRANSFER_MAX_SIZE", 1024)

    # HTTP server for config web UI (port 81)
    add_idf_sdkconfig_option("CONFIG_HTTPD_MAX_REQ_HDR_LEN", 1024)
    # Default lwIP socket count is low; TCP listeners + httpd + HA polling can hit ENFILE (errno 23) on accept().
    add_idf_sdkconfig_option("CONFIG_LWIP_MAX_SOCKETS", 20)

    # Enable native hub support
    add_idf_sdkconfig_option("CONFIG_USB_HOST_HUBS_SUPPORTED", True)
    add_idf_sdkconfig_option("CONFIG_USB_HOST_HUB_MULTI_LEVEL", True)

    # PSRAM can cause USB host interrupts to be missed (ESP-IDF #9519).
    add_idf_sdkconfig_option("CONFIG_SPIRAM", False)
    add_idf_sdkconfig_option("CONFIG_ESP32S3_SPIRAM_SUPPORT", False)
