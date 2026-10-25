"""Async receiver for a USB CDC channel provided by ``usb_uart``.

``usb_uart`` hands out ``UARTComponent`` channels but has no way of telling
anybody that data arrived: with no consumer, ``read_array()`` is never called, so
its RX transfer is never restarted after a failure (e.g. one light-sleep glitch)
and the channel goes silent until the device is replugged.

This component is that consumer. Every main-loop iteration it drains the channel
(non-blocking) and:

* keeps the RX path alive -- ``read_array()`` is what restarts a stalled transfer
  in ``usb_uart`` (``usb_uart.cpp:211``), with a rate-limited nudge so a dead
  transfer recovers on its own;
* fires ``on_data`` for every batch of bytes and, when a delimiter is
  configured, ``on_line`` for every complete line.

Both triggers pass a ``std::string`` (``x``), so binary data works too (index
``x[i]`` if you need raw bytes).
"""

import esphome.automation as automation
import esphome.codegen as cg
from esphome.components import uart
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_TRIGGER_ID

CODEOWNERS = ["@Cookie987"]
DEPENDENCIES = ["uart"]

CONF_DELIMITER = "delimiter"
CONF_MAX_LINE_LENGTH = "max_line_length"
CONF_ON_DATA = "on_data"
CONF_ON_LINE = "on_line"

usb_cdc_rx_ns = cg.esphome_ns.namespace("usb_cdc_rx")
USBCdcRx = usb_cdc_rx_ns.class_("USBCdcRx", cg.Component, uart.UARTDevice)
DataTrigger = usb_cdc_rx_ns.class_("DataTrigger", automation.Trigger.template(cg.std_string))
LineTrigger = usb_cdc_rx_ns.class_("LineTrigger", automation.Trigger.template(cg.std_string))

CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(USBCdcRx),
            # 空字符串 = 关闭行模式（只走 on_data / raw listener）
            cv.Optional(CONF_DELIMITER, default="\n"): cv.string,
            cv.Optional(CONF_MAX_LINE_LENGTH, default=256): cv.int_range(min=8, max=4096),
            cv.Optional(CONF_ON_DATA): automation.validate_automation(
                {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(DataTrigger)}
            ),
            cv.Optional(CONF_ON_LINE): automation.validate_automation(
                {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(LineTrigger)}
            ),
        }
    )
    .extend(uart.UART_DEVICE_SCHEMA)
    .extend(cv.COMPONENT_SCHEMA)
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await uart.register_uart_device(var, config)

    cg.add(var.set_delimiter(config[CONF_DELIMITER]))
    cg.add(var.set_max_line_length(config[CONF_MAX_LINE_LENGTH]))

    for conf in config.get(CONF_ON_DATA, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [(cg.std_string, "x")], conf)
    for conf in config.get(CONF_ON_LINE, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [(cg.std_string, "x")], conf)
