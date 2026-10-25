#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "esphome/components/uart/uart.h"
#include "esphome/core/automation.h"
#include "esphome/core/component.h"

namespace esphome::usb_cdc_rx {

class USBCdcRx : public Component, public uart::UARTDevice {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override;

  void set_delimiter(const std::string &delimiter) { this->delimiter_ = delimiter; }
  void set_max_line_length(size_t max_line_length) { this->max_line_length_ = max_line_length; }

  void add_on_data_trigger(Trigger<std::string> *trigger) { this->data_triggers_.push_back(trigger); }
  void add_on_line_trigger(Trigger<std::string> *trigger) { this->line_triggers_.push_back(trigger); }

  /// Register a raw byte listener (C-level consumer, e.g. the Lua runtime bridge).
  /// Called from the main loop for every batch of received bytes.
  void add_raw_listener(std::function<void(const uint8_t *, size_t)> listener) {
    this->raw_listeners_.push_back(std::move(listener));
  }

  /// Send bytes to the USB CDC device. MUST be called from the main loop
  /// (usb_uart's output queue is single-producer).
  void write_bytes(const uint8_t *data, size_t len) { this->write_array(data, len); }

  /// True once usb_uart has finished bringing the channel up (device enumerated).
  bool device_connected();

 protected:
  /// Drain everything the channel currently holds. Never blocks.
  void drain_();
  /// Hand one batch of bytes to the triggers (and feed the line splitter).
  void dispatch_(const uint8_t *data, size_t len);
  void emit_line_();

  std::vector<Trigger<std::string> *> data_triggers_;
  std::vector<Trigger<std::string> *> line_triggers_;
  std::vector<std::function<void(const uint8_t *, size_t)>> raw_listeners_;

  std::string delimiter_{"\n"};
  std::string line_buffer_;
  size_t max_line_length_{256};
  uint32_t last_nudge_{0};
  bool draining_{false};
};

class DataTrigger : public Trigger<std::string> {
 public:
  explicit DataTrigger(USBCdcRx *parent) { parent->add_on_data_trigger(this); }
};

class LineTrigger : public Trigger<std::string> {
 public:
  explicit LineTrigger(USBCdcRx *parent) { parent->add_on_line_trigger(this); }
};

}  // namespace esphome::usb_cdc_rx
