#include "websocket_client.h"

#include <Arduino.h>

#include "esphome/components/network/util.h"
#include "esphome/core/log.h"

namespace esphome {
namespace websocket_client {

static const char *const TAG = "websocket_client";

void WebsocketClient::setup() {
  this->client_.onMessage([this](websockets::WebsocketsMessage message) {
    if (!message.isText()) {
      ESP_LOGV(TAG, "Ignoring non-text WebSocket frame");
      return;
    }

    const std::string payload = message.data().c_str();
    ESP_LOGD(TAG, "Received message: %s", payload.c_str());
    this->message_callback_.call(payload);
  });

  this->client_.onEvent([this](websockets::WebsocketsEvent event, String data) {
    switch (event) {
      case websockets::WebsocketsEvent::ConnectionOpened:
        ESP_LOGI(TAG, "WebSocket connected");
        this->handle_connected_();
        break;
      case websockets::WebsocketsEvent::ConnectionClosed:
        ESP_LOGW(TAG, "WebSocket disconnected");
        this->handle_disconnected_();
        break;
      case websockets::WebsocketsEvent::GotPing:
        ESP_LOGVV(TAG, "Received ping");
        break;
      case websockets::WebsocketsEvent::GotPong:
        ESP_LOGVV(TAG, "Received pong");
        break;
    }
  });

  if (this->auto_connect_) {
    this->connect_requested_ = true;
    this->attempt_connect_();
  }
}

void WebsocketClient::loop() {
  if (this->client_.available()) {
    this->client_.poll();
  } else if (this->connected_) {
    this->handle_disconnected_();
  }

  if (!this->should_connect_() || this->connected_ || !network::is_connected()) {
    return;
  }

  const uint32_t now = millis();
  if (now - this->last_connect_attempt_ < this->reconnect_interval_) {
    return;
  }

  this->attempt_connect_();
}

void WebsocketClient::dump_config() {
  ESP_LOGCONFIG(TAG, "WebSocket Client:");
  ESP_LOGCONFIG(TAG, "  URL: %s", this->url_.c_str());
  ESP_LOGCONFIG(TAG, "  Auto Connect: %s", YESNO(this->auto_connect_));
  ESP_LOGCONFIG(TAG, "  Reconnect Interval: %.1fs", this->reconnect_interval_ / 1000.0f);
}

void WebsocketClient::connect() {
  this->connect_requested_ = true;
  if (!this->connected_) {
    this->attempt_connect_();
  }
}

void WebsocketClient::disconnect() {
  if (!this->auto_connect_) {
    this->connect_requested_ = false;
  }

  if (this->connected_ || this->client_.available()) {
    this->client_.close();
  }
  this->handle_disconnected_();
}

bool WebsocketClient::send(const std::string &message) {
  if (!this->connected_) {
    ESP_LOGW(TAG, "Cannot send WebSocket message while disconnected");
    this->status_momentary_warning("send", 5000);
    return false;
  }

  const bool sent = this->client_.send(message.c_str());
  if (!sent) {
    ESP_LOGW(TAG, "Failed to send WebSocket message");
    this->status_momentary_warning("send", 5000);
  }
  return sent;
}

void WebsocketClient::attempt_connect_() {
  if (!network::is_connected() || this->url_.empty()) {
    return;
  }

  this->last_connect_attempt_ = millis();
  ESP_LOGI(TAG, "Connecting to %s", this->url_.c_str());

  if (this->client_.connect(this->url_.c_str())) {
    if (!this->connected_) {
      this->handle_connected_();
    }
    return;
  }

  ESP_LOGW(TAG, "WebSocket connection failed");
  this->status_set_warning(LOG_STR("WebSocket connect failed"));
  this->handle_disconnected_();
}

void WebsocketClient::handle_connected_() {
  if (this->connected_) {
    return;
  }

  this->connected_ = true;
  this->status_clear_warning();
  this->connect_callback_.call();
}

void WebsocketClient::handle_disconnected_() {
  if (!this->connected_) {
    return;
  }

  this->connected_ = false;
  this->status_set_warning(LOG_STR("WebSocket disconnected"));
  this->disconnect_callback_.call();
}

bool WebsocketClient::should_connect_() const { return this->auto_connect_ || this->connect_requested_; }

}  // namespace websocket_client
}  // namespace esphome
