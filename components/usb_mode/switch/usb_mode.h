#pragma once

#include "esphome/components/switch/switch.h"
#include "esphome/core/component.h"

#ifdef CONFIG_PM_ENABLE
#include <esp_pm.h>
#endif

namespace esphome::usb_mode {

/// Moves the ESP32-S3's internal USB PHY between the USB-OTG controller
/// (host mode, owned by the usb_host/usb_uart components) and the
/// USB-Serial-JTAG peripheral (device mode, used by the ESPHome logger).
///
/// ON  = host mode   -> the Type-C port reads an attached USB CDC device
/// OFF = device mode -> the Type-C port presents the USB-Serial-JTAG console
///
/// USB cannot survive light sleep: as soon as ESP-IDF's power management puts
/// the chip to sleep, pending USB transfers fail with USB_TRANSFER_STATUS_ERROR
/// and the attached device drops off the bus. In host mode this component
/// therefore holds ESP_PM_APB_FREQ_MAX + ESP_PM_NO_LIGHT_SLEEP (see
/// allow_light_sleep), which pins the PM mode floor above PM_MODE_LIGHT_SLEEP.
class USBModeSwitch : public switch_::Switch, public Component {
 public:
  void setup() override;
  void dump_config() override;
  float get_setup_priority() const override;

  void set_reboot_on_switch(bool reboot_on_switch) { this->reboot_on_switch_ = reboot_on_switch; }
  void set_allow_light_sleep(bool allow_light_sleep) { this->allow_light_sleep_ = allow_light_sleep; }

 protected:
  void write_state(bool state) override;
  void select_phy_(bool host_mode);
  void acquire_pm_locks_();
  void release_pm_locks_();

  bool reboot_on_switch_{false};
  bool applying_boot_state_{false};
  bool allow_light_sleep_{false};
  bool pm_locks_held_{false};
#ifdef CONFIG_PM_ENABLE
  esp_pm_lock_handle_t apb_lock_{nullptr};
  esp_pm_lock_handle_t no_sleep_lock_{nullptr};
#endif
};

}  // namespace esphome::usb_mode
