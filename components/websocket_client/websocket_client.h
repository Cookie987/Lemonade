#pragma once

#include <ArduinoWebsockets.h>

#include <string>

#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"

namespace esphome {
namespace websocket_client {

class WebsocketClient : public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

  void set_url(const std::string &url) { this->url_ = url; }
  void set_auto_connect(bool auto_connect) { this->auto_connect_ = auto_connect; }
  void set_reconnect_interval(uint32_t reconnect_interval) { this->reconnect_interval_ = reconnect_interval; }

  void connect();
  void disconnect();
  bool send(const std::string &message);
  bool is_connected() const { return this->connected_; }

  void add_on_message_callback(std::function<void(std::string)> callback) {
    this->message_callback_.add(std::move(callback));
  }
  void add_on_connect_callback(std::function<void()> callback) { this->connect_callback_.add(std::move(callback)); }
  void add_on_disconnect_callback(std::function<void()> callback) {
    this->disconnect_callback_.add(std::move(callback));
  }

 protected:
  void attempt_connect_();
  void handle_connected_();
  void handle_disconnected_();
  bool should_connect_() const;

  websockets::WebsocketsClient client_;
  std::string url_;
  CallbackManager<void(std::string)> message_callback_;
  CallbackManager<void()> connect_callback_;
  CallbackManager<void()> disconnect_callback_;
  uint32_t reconnect_interval_{5000};
  uint32_t last_connect_attempt_{0};
  bool auto_connect_{true};
  bool connect_requested_{false};
  bool connected_{false};
};

class WebsocketClientMessageTrigger : public Trigger<std::string> {
 public:
  explicit WebsocketClientMessageTrigger(WebsocketClient *parent) {
    parent->add_on_message_callback([this](const std::string &message) { this->trigger(message); });
  }
};

class WebsocketClientConnectTrigger : public Trigger<> {
 public:
  explicit WebsocketClientConnectTrigger(WebsocketClient *parent) {
    parent->add_on_connect_callback([this]() { this->trigger(); });
  }
};

class WebsocketClientDisconnectTrigger : public Trigger<> {
 public:
  explicit WebsocketClientDisconnectTrigger(WebsocketClient *parent) {
    parent->add_on_disconnect_callback([this]() { this->trigger(); });
  }
};

template<typename... Ts> class WebsocketClientSendAction : public Action<Ts...> {
 public:
  explicit WebsocketClientSendAction(WebsocketClient *parent) : parent_(parent) {}
  TEMPLATABLE_VALUE(std::string, message)

  void play(const Ts &...x) override { this->parent_->send(this->message_.value(x...)); }

 protected:
  WebsocketClient *parent_;
};

template<typename... Ts> class WebsocketClientConnectAction : public Action<Ts...> {
 public:
  explicit WebsocketClientConnectAction(WebsocketClient *parent) : parent_(parent) {}

  void play(const Ts &...x) override { this->parent_->connect(); }

 protected:
  WebsocketClient *parent_;
};

template<typename... Ts> class WebsocketClientDisconnectAction : public Action<Ts...> {
 public:
  explicit WebsocketClientDisconnectAction(WebsocketClient *parent) : parent_(parent) {}

  void play(const Ts &...x) override { this->parent_->disconnect(); }

 protected:
  WebsocketClient *parent_;
};

template<typename... Ts> class WebsocketClientConnectedCondition : public Condition<Ts...> {
 public:
  explicit WebsocketClientConnectedCondition(WebsocketClient *parent) : parent_(parent) {}

  bool check(const Ts &...x) override { return this->parent_->is_connected(); }

 protected:
  WebsocketClient *parent_;
};

}  // namespace websocket_client
}  // namespace esphome
