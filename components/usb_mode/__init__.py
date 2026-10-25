"""ESP32-S3 USB role switch: device (log) mode <-> host mode, at runtime.

The ESP32-S3 has a single internal USB FSLS PHY shared between the USB-OTG
controller (used by ``usb_host``/``usb_uart`` in host mode) and the
USB-Serial-JTAG peripheral (used by the ESPHome logger in device/log mode).
Only one of them can own the PHY at a time, so this component arbitrates it with
a switch entity:

* switch OFF (default) -> device mode: the Type-C port is the USB-Serial-JTAG
  console, so ESPHome logs show up on the PC as a COM port.
* switch ON            -> host mode: the Type-C port reads an attached USB CDC
  device through ``usb_uart``.

The switch is in ``switch.py``/``switch/`` (platform ``usb_mode``).
"""

import esphome.codegen as cg

CODEOWNERS = ["@Cookie987"]

usb_mode_ns = cg.esphome_ns.namespace("usb_mode")
