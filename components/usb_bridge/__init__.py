import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID, ENTITY_CATEGORY_DIAGNOSTIC
from esphome.components.esp32 import add_idf_sdkconfig_option
from esphome.components import sensor, text_sensor

DEPENDENCIES = ["network"]
AUTO_LOAD = ["sensor", "text_sensor"]
CODEOWNERS = []

usb_bridge_ns = cg.esphome_ns.namespace("usb_bridge")
UsbBridgeComponent = usb_bridge_ns.class_("UsbBridgeComponent", cg.Component)

CONF_DEVICES_SENSOR = "devices_connected"
CONF_FIRMWARE_SENSOR = "firmware"
CONF_CONFIG_URL_SENSOR = "config_url"
CONF_IP_ADDRESS_SENSOR = "ip_address"

# Per-device sensors (up to 8 slots)
MAX_DEVICE_SLOTS = 8
CONF_DEVICE_NAME = "device_name"
CONF_DEVICE_PORT = "device_port"
CONF_DEVICE_STATUS = "device_status"

DEVICE_SENSOR_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_DEVICE_NAME): text_sensor.text_sensor_schema(
            icon="mdi:usb",
        ),
        cv.Required(CONF_DEVICE_PORT): text_sensor.text_sensor_schema(
            icon="mdi:lan-connect",
        ),
        cv.Required(CONF_DEVICE_STATUS): text_sensor.text_sensor_schema(
            icon="mdi:lan-check",
        ),
    }
)

CONF_DEVICES = "devices"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(UsbBridgeComponent),
        cv.Optional(CONF_DEVICES_SENSOR): sensor.sensor_schema(
            icon="mdi:usb",
            accuracy_decimals=0,
            state_class="measurement",
        ),
        cv.Optional(CONF_FIRMWARE_SENSOR): text_sensor.text_sensor_schema(
            icon="mdi:chip",
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ),
        cv.Optional(CONF_CONFIG_URL_SENSOR): text_sensor.text_sensor_schema(
            icon="mdi:web",
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ),
        cv.Optional(CONF_IP_ADDRESS_SENSOR): text_sensor.text_sensor_schema(
            icon="mdi:ip-network",
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ),
        cv.Optional(CONF_DEVICES): cv.All(
            cv.ensure_list(DEVICE_SENSOR_SCHEMA),
            cv.Length(max=MAX_DEVICE_SLOTS),
        ),
    }
).extend(cv.COMPONENT_SCHEMA)

async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    # Create sensors via codegen (proper ESPHome way)
    if CONF_DEVICES_SENSOR in config:
        sens = await sensor.new_sensor(config[CONF_DEVICES_SENSOR])
        cg.add(var.set_devices_sensor(sens))

    if CONF_FIRMWARE_SENSOR in config:
        sens = await text_sensor.new_text_sensor(config[CONF_FIRMWARE_SENSOR])
        cg.add(var.set_firmware_sensor(sens))

    if CONF_CONFIG_URL_SENSOR in config:
        sens = await text_sensor.new_text_sensor(config[CONF_CONFIG_URL_SENSOR])
        cg.add(var.set_config_url_sensor(sens))

    if CONF_IP_ADDRESS_SENSOR in config:
        sens = await text_sensor.new_text_sensor(config[CONF_IP_ADDRESS_SENSOR])
        cg.add(var.set_ip_address_sensor(sens))

    # Per-device sensor slots
    if CONF_DEVICES in config:
        for i, dev_conf in enumerate(config[CONF_DEVICES]):
            name_sens = await text_sensor.new_text_sensor(dev_conf[CONF_DEVICE_NAME])
            cg.add(var.set_device_name_sensor(i, name_sens))
            port_sens = await text_sensor.new_text_sensor(dev_conf[CONF_DEVICE_PORT])
            cg.add(var.set_device_port_sensor(i, port_sens))
            status_sens = await text_sensor.new_text_sensor(dev_conf[CONF_DEVICE_STATUS])
            cg.add(var.set_device_status_sensor(i, status_sens))

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
    # Required: enum_filter_cb field only exists in usb_host_config_t when enabled.
    # Without this, the struct layout is wrong and usb_host_install() silently fails.
    add_idf_sdkconfig_option("CONFIG_USB_HOST_ENABLE_ENUM_FILTER_CALLBACK", True)

    # PSRAM can cause USB host interrupts to be missed (ESP-IDF #9519).
    add_idf_sdkconfig_option("CONFIG_SPIRAM", False)
    add_idf_sdkconfig_option("CONFIG_ESP32S3_SPIRAM_SUPPORT", False)
