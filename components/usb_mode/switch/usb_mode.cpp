#include "usb_mode.h"

#ifdef USE_ESP32
#include "esphome/core/application.h"
#include "esphome/core/log.h"

#if defined(USE_ESP32_VARIANT_ESP32S3)
// ESP32-S3 only: the internal USB FSLS PHY is shared between the USB-OTG
// (USB_WRAP) block and the USB-Serial-JTAG (USJ) peripheral. Which peripheral
// owns it is selected in software, via RTCCNTL.usb_conf.sw_hw_usb_phy_sel /
// sw_usb_phy_sel - the very same bits ESP-IDF's own USB drivers write when
// they initialise (see usb_wrap_hal_phy_set_external() and
// usb_serial_jtag_driver_install()).
#include "hal/usb_serial_jtag_ll.h"
#include "hal/usb_wrap_ll.h"
#endif

namespace esphome::usb_mode {

static const char *const TAG = "usb_mode";

#if defined(USE_ESP32_VARIANT_ESP32S3)

// Run after the usb_host component claimed the PHY in USBHost::setup()
// (setup_priority::BUS) but before usb_uart's USBClient::setup()
// (setup_priority::IO) registers its client, so the client registration is
// never disturbed by the mode we pick.
float USBModeSwitch::get_setup_priority() const { return setup_priority::BUS - 1.0f; }

void USBModeSwitch::setup() {
  // The switch state is restored from flash by the switch base class
  // (restore_mode: RESTORE_DEFAULT_OFF), so write_state() is what selects the
  // PHY at boot - just without triggering the optional reboot.
  this->applying_boot_state_ = true;
  this->write_state(this->get_initial_state_with_restore_mode().value_or(false));
  this->applying_boot_state_ = false;
}

void USBModeSwitch::write_state(bool state) {
  this->select_phy_(state);
  this->publish_state(state);

  if (this->reboot_on_switch_ && !this->applying_boot_state_) {
    ESP_LOGI(TAG, "USB mode changed, rebooting to apply it cleanly");
    App.safe_reboot();
  }
}

// USB has no chance against light sleep: with ESP-IDF power management enabled
// (power_management / tickless idle), a pending USB transfer dies with
// USB_TRANSFER_STATUS_ERROR and the attached device drops off the bus the moment
// the chip sleeps. IDF's own usb_host takes no PM locks, so we take them here.
// ESP_PM_APB_FREQ_MAX pins the PM mode floor to PM_MODE_APB_MAX and
// ESP_PM_NO_LIGHT_SLEEP pins it to PM_MODE_APB_MIN - both are above
// PM_MODE_LIGHT_SLEEP, so the PM algorithm stops choosing light sleep.
void USBModeSwitch::acquire_pm_locks_() {
#ifdef CONFIG_PM_ENABLE
  if (this->allow_light_sleep_ || this->pm_locks_held_)
    return;

  if (this->apb_lock_ == nullptr &&
      esp_pm_lock_create(ESP_PM_APB_FREQ_MAX, 0, "usb_mode_apb", &this->apb_lock_) != ESP_OK) {
    this->apb_lock_ = nullptr;
  }
  if (this->no_sleep_lock_ == nullptr &&
      esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "usb_mode_slp", &this->no_sleep_lock_) != ESP_OK) {
    this->no_sleep_lock_ = nullptr;
  }
  if (this->apb_lock_ == nullptr && this->no_sleep_lock_ == nullptr) {
    ESP_LOGW(TAG, "Could not create power management locks - USB will break when the device sleeps");
    return;
  }
  if (this->apb_lock_ != nullptr)
    esp_pm_lock_acquire(this->apb_lock_);
  if (this->no_sleep_lock_ != nullptr)
    esp_pm_lock_acquire(this->no_sleep_lock_);
  this->pm_locks_held_ = true;
  ESP_LOGI(TAG, "PM locks held (APB max + no light sleep) - required while USB host mode is active");
#endif
}

void USBModeSwitch::release_pm_locks_() {
#ifdef CONFIG_PM_ENABLE
  if (!this->pm_locks_held_)
    return;
  if (this->apb_lock_ != nullptr)
    esp_pm_lock_release(this->apb_lock_);
  if (this->no_sleep_lock_ != nullptr)
    esp_pm_lock_release(this->no_sleep_lock_);
  this->pm_locks_held_ = false;
  ESP_LOGI(TAG, "PM locks released - light sleep is allowed again");
#endif
}

void USBModeSwitch::select_phy_(bool host_mode) {
  if (host_mode) {
    // Take the PM locks *before* the PHY so the APB clock stays put during the
    // switch, and so nothing can put the chip to sleep while the host is live.
    this->acquire_pm_locks_();
    // Take the PHY for the USB-OTG controller. Mirrors what
    // usb_wrap_hal_phy_set_external(hal, false) plus
    // usb_phy_otg_set_mode(USB_OTG_MODE_HOST) configure: internal PHY routed to
    // the USB_WRAP block and the 15k pull-downs a host must present on D+/D-.
    usb_serial_jtag_ll_phy_disable_pull_override();
    usb_wrap_pull_override_vals_t vals{};
    vals.dp_pu = false;
    vals.dp_pd = true;
    vals.dm_pu = false;
    vals.dm_pd = true;
    usb_wrap_ll_phy_enable_pull_override(&USB_WRAP, &vals);
    usb_wrap_ll_phy_enable_pad(&USB_WRAP, true);
    // Also sets sw_hw_usb_phy_sel = 1, sw_usb_phy_sel = 1 (PHY -> USB_WRAP).
    usb_wrap_ll_phy_enable_external(&USB_WRAP, false);
    ESP_LOGI(TAG, "USB PHY handed to the USB-OTG controller (host mode)");
  } else {
    // Give the PHY back to the USB-Serial-JTAG peripheral so the console
    // (logger: hardware_uart: USB_SERIAL_JTAG) works again. The host gets no
    // PHY while in this mode, so any attached USB device stays invisible.
    usb_wrap_ll_phy_disable_pull_override(&USB_WRAP);
    // Also sets sw_hw_usb_phy_sel = 1, sw_usb_phy_sel = 0 (PHY -> USJ).
    usb_serial_jtag_ll_phy_enable_external(false);
    usb_serial_jtag_ll_phy_enable_pad(true);
    this->release_pm_locks_();
    ESP_LOGI(TAG, "USB PHY handed to USB-Serial-JTAG (device / log mode)");
  }
}

void USBModeSwitch::dump_config() {
  LOG_SWITCH("", "USB Mode Switch", this);
  ESP_LOGCONFIG(TAG,
                "  Mode: %s\n"
                "  Reboot on switch: %s\n"
                "  Allow light sleep in host mode: %s%s",
                this->state ? "host (USB-OTG reads the attached device)" : "device (USB-Serial-JTAG logs)",
                YESNO(this->reboot_on_switch_), YESNO(this->allow_light_sleep_),
                this->pm_locks_held_ ? " (PM locks held)" : "");
}

#else  // not ESP32-S3

float USBModeSwitch::get_setup_priority() const { return setup_priority::BUS - 1.0f; }

void USBModeSwitch::setup() {
  this->publish_state(false);
  ESP_LOGE(TAG, "usb_mode only supports the ESP32-S3 (shared USB FSLS PHY)");
  this->mark_failed();
}

void USBModeSwitch::write_state(bool state) { this->publish_state(state); }
void USBModeSwitch::select_phy_(bool) {}
void USBModeSwitch::dump_config() { LOG_SWITCH("", "USB Mode Switch", this); }

#endif  // USE_ESP32_VARIANT_ESP32S3

}  // namespace esphome::usb_mode
#endif  // USE_ESP32
