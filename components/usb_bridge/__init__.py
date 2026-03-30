import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID
from esphome.components.esp32 import add_idf_sdkconfig_option

DEPENDENCIES = ["network"]
CODEOWNERS = []

usb_bridge_ns = cg.esphome_ns.namespace("usb_bridge")
UsbBridgeComponent = usb_bridge_ns.class_("UsbBridgeComponent", cg.Component)

CONF_REJECT_SECOND_CP210X = "reject_second_cp210x"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(UsbBridgeComponent),
        cv.Optional(CONF_REJECT_SECOND_CP210X, default=False): cv.boolean,
    }
).extend(cv.COMPONENT_SCHEMA)

async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_reject_second_cp210x(config[CONF_REJECT_SECOND_CP210X]))

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
    # Needed when enum_filter_cb is used (reject_second_cp210x)
    add_idf_sdkconfig_option("CONFIG_USB_HOST_ENABLE_ENUM_FILTER_CALLBACK", True)

    # PSRAM can cause USB host interrupts to be missed (ESP-IDF #9519).
    add_idf_sdkconfig_option("CONFIG_SPIRAM", False)
    add_idf_sdkconfig_option("CONFIG_ESP32S3_SPIRAM_SUPPORT", False)
