#include "usb_cdc_rx.h"

#include <algorithm>
#include <cstdio>

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

namespace esphome::usb_cdc_rx {

static const char *const TAG = "usb_cdc_rx";

// usb_uart only restarts a failed IN transfer from read_array(); without a
// consumer the RX path stays dead after the first error (one light-sleep glitch
// is enough). Nudge it at this interval while the channel is idle so it heals
// by itself instead of needing a replug.
static constexpr uint32_t NUDGE_INTERVAL_MS = 250;
static constexpr size_t READ_CHUNK = 64;

float USBCdcRx::get_setup_priority() const { return setup_priority::DATA; }

void USBCdcRx::setup() { this->drain_(); }

void USBCdcRx::loop() { this->drain_(); }

void USBCdcRx::drain_() {
  if (this->draining_)
    return;
  this->draining_ = true;

  uint8_t buffer[READ_CHUNK];
  while (true) {
    size_t available = this->available();
    if (available == 0) {
      uint32_t now = millis();
      if (now - this->last_nudge_ >= NUDGE_INTERVAL_MS) {
        this->last_nudge_ = now;
        // Zero-length read: reads nothing but calls USBUartComponent::start_input(),
        // which is what re-arms a transfer that failed earlier.
        uint8_t dummy;
        this->read_array(&dummy, 0);
      }
      break;
    }
    size_t want = std::min(available, sizeof(buffer));
    this->read_array(buffer, want);
    this->dispatch_(buffer, want);
  }

  this->draining_ = false;
}

void USBCdcRx::dispatch_(const uint8_t *data, size_t len) {
  for (auto &listener : this->raw_listeners_)
    listener(data, len);

  if (!this->data_triggers_.empty()) {
    std::string chunk(reinterpret_cast<const char *>(data), len);
    for (auto *trigger : this->data_triggers_)
      trigger->trigger(chunk);
  }

  if (this->line_triggers_.empty())
    return;

  const size_t delimiter_len = this->delimiter_.size();
  for (size_t i = 0; i < len; i++) {
    this->line_buffer_.push_back(static_cast<char>(data[i]));
    if (delimiter_len != 0 && this->line_buffer_.size() >= delimiter_len &&
        this->line_buffer_.compare(this->line_buffer_.size() - delimiter_len, delimiter_len, this->delimiter_) == 0) {
      this->line_buffer_.resize(this->line_buffer_.size() - delimiter_len);
      this->emit_line_();
    } else if (this->line_buffer_.size() >= this->max_line_length_) {
      // No delimiter in sight - hand out what we have instead of growing forever.
      this->emit_line_();
    }
  }
}

void USBCdcRx::emit_line_() {
  std::string line;
  line.swap(this->line_buffer_);
  // Serial text usually ends with "\r\n"; keep the payload clean for CRLF devices.
  if (!line.empty() && line.back() == '\r')
    line.pop_back();
  for (auto *trigger : this->line_triggers_)
    trigger->trigger(line);
}

bool USBCdcRx::device_connected() { return this->parent_ != nullptr && this->parent_->is_connected(); }

void USBCdcRx::dump_config() {
  ESP_LOGCONFIG(TAG, "USB CDC receiver:");

  std::string delimiter;
  if (this->delimiter_.empty()) {
    delimiter = "<none>";
  } else {
    for (char c : this->delimiter_) {
      if (c == '\n') {
        delimiter += "\\n";
      } else if (c == '\r') {
        delimiter += "\\r";
      } else if (c == '\t') {
        delimiter += "\\t";
      } else if (c >= 0x20 && c < 0x7F) {
        delimiter += c;
      } else {
        char escaped[8];
        snprintf(escaped, sizeof(escaped), "\\x%02X", static_cast<uint8_t>(c));
        delimiter += escaped;
      }
    }
  }
  ESP_LOGCONFIG(TAG,
                "  Delimiter: '%s'\n"
                "  Max line length: %u\n"
                "  on_data automations: %u\n"
                "  on_line automations: %u",
                delimiter.c_str(), static_cast<unsigned>(this->max_line_length_),
                static_cast<unsigned>(this->data_triggers_.size()),
                static_cast<unsigned>(this->line_triggers_.size()));
}

}  // namespace esphome::usb_cdc_rx
