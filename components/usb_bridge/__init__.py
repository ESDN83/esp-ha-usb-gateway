import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_BAUD_RATE, CONF_NAME, CONF_PORT
from esphome.components.esp32 import add_idf_sdkconfig_option

DEPENDENCIES = ["network"]
CODEOWNERS = []

CONF_DEVICES = "devices"
CONF_VID = "vid"
CONF_PID = "pid"
CONF_INTERFACE = "interface"
CONF_AUTOBOOT = "autoboot"

usb_bridge_ns = cg.esphome_ns.namespace("usb_bridge")
UsbBridgeComponent = usb_bridge_ns.class_("UsbBridgeComponent", cg.Component)
DeviceConfig = usb_bridge_ns.struct("DeviceConfig")

def hex_uint16_t(value):
    if isinstance(value, str):
        value = value.strip().lower()
        if not value.startswith("0x"):
            value = "0x" + value
        return int(value, 16)
    return cv.int_(value)

DEVICE_SCHEMA = cv.Schema({
    cv.Required(CONF_NAME): cv.string,
    cv.Required(CONF_VID): hex_uint16_t,
    cv.Required(CONF_PID): hex_uint16_t,
    cv.Required(CONF_PORT): cv.port,
    cv.Optional(CONF_BAUD_RATE, default=115200): cv.positive_int,
    cv.Optional(CONF_INTERFACE, default=0): cv.uint8_t,
    cv.Optional(CONF_AUTOBOOT, default=False): cv.boolean,
})

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(UsbBridgeComponent),
        cv.Required(CONF_DEVICES): cv.ensure_list(DEVICE_SCHEMA),
    }
).extend(cv.COMPONENT_SCHEMA)

async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    for dev in config[CONF_DEVICES]:
        cg.add(var.add_device_config(
            dev[CONF_VID],
            dev[CONF_PID],
            dev[CONF_PORT],
            dev[CONF_BAUD_RATE],
            dev[CONF_INTERFACE],
            dev[CONF_AUTOBOOT]
        ))

    # Bare minimum for USB host on ESP32-S3
    add_idf_sdkconfig_option("CONFIG_USB_OTG_SUPPORTED", True)
    add_idf_sdkconfig_option("CONFIG_USB_HOST_CONTROL_TRANSFER_MAX_SIZE", 1024)

    # Enable native hub support
    add_idf_sdkconfig_option("CONFIG_USB_HOST_HUBS_SUPPORTED", True)
    add_idf_sdkconfig_option("CONFIG_USB_HOST_HUB_MULTI_LEVEL", True)

    # PSRAM can cause USB host interrupts to be missed (ESP-IDF #9519).
    # Disable PSRAM to ensure reliable USB host operation.
    add_idf_sdkconfig_option("CONFIG_SPIRAM", False)
    add_idf_sdkconfig_option("CONFIG_ESP32S3_SPIRAM_SUPPORT", False)
