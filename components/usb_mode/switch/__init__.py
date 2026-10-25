import esphome.codegen as cg
from esphome.components import esp32, switch
from esphome.components.esp32 import VARIANT_ESP32S3
import esphome.config_validation as cv
from esphome import final_validate as fv
from esphome.const import CONF_HARDWARE_UART

from .. import usb_mode_ns

DEPENDENCIES = ["esp32"]

CONF_REBOOT_ON_SWITCH = "reboot_on_switch"
CONF_ALLOW_LIGHT_SLEEP = "allow_light_sleep"

USBModeSwitch = usb_mode_ns.class_("USBModeSwitch", switch.Switch, cg.Component)

CONFIG_SCHEMA = cv.All(
    switch.switch_schema(
        USBModeSwitch,
        default_restore_mode="RESTORE_DEFAULT_OFF",
        icon="mdi:usb-port",
    )
    .extend(
        {
            cv.Optional(CONF_REBOOT_ON_SWITCH, default=False): cv.boolean,
            # USB dies in light sleep (pending transfers fail, the device drops
            # off the bus), so host mode holds ESP_PM_APB_FREQ_MAX +
            # ESP_PM_NO_LIGHT_SLEEP unless this is explicitly allowed.
            cv.Optional(CONF_ALLOW_LIGHT_SLEEP, default=False): cv.boolean,
        }
    )
    .extend(cv.COMPONENT_SCHEMA),
    esp32.only_on_variant(supported=[VARIANT_ESP32S3]),
)


async def to_code(config):
    var = await switch.new_switch(config)
    await cg.register_component(var, config)
    cg.add(var.set_reboot_on_switch(config[CONF_REBOOT_ON_SWITCH]))
    cg.add(var.set_allow_light_sleep(config[CONF_ALLOW_LIGHT_SLEEP]))


def _final_validate(config):
    """Device mode hands the shared PHY to the USB-Serial-JTAG peripheral, so the
    logger has to be on that same peripheral."""
    logger_config = fv.full_config.get().get("logger")
    if logger_config is None:
        return config
    hardware_uart = logger_config.get(CONF_HARDWARE_UART)
    if hardware_uart is None or "USB_SERIAL_JTAG" in str(hardware_uart):
        return config
    raise cv.Invalid(
        "usb_mode needs the ESPHome logger on the USB-Serial-JTAG peripheral, "
        "otherwise the device (log) mode has no USB output: add "
        "'logger: hardware_uart: USB_SERIAL_JTAG'.",
        ["logger", CONF_HARDWARE_UART],
    )


FINAL_VALIDATE_SCHEMA = _final_validate
