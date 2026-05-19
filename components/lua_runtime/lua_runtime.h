#pragma once

#include <functional>
#include <string>

#include "esphome/core/automation.h"
#include "esphome/core/component.h"

struct lua_State;

namespace esphome {
namespace rtttl {
class Rtttl;
}  // namespace rtttl
namespace media_player {
class MediaPlayer;
}  // namespace media_player
}  // namespace esphome

namespace esphome {
namespace lua_runtime {

bool is_active_app_page();
void lua_abort_if_ota(lua_State *L);

class LuaRuntime : public Component {
 public:
  void setup() override;
  void dump_config() override;
  void loop() override;
  bool run_file(const std::string &path);
  bool run_file_async(const std::string &path);
  void mark_task_done(const std::string &path);
  void abort_all();
  void set_async_core(int async_core) { this->async_core_ = async_core; }
  void set_ota_active(bool active);
  void register_rtttl_player(const std::string &name, rtttl::Rtttl *player);
  void register_media_player(const std::string &name, media_player::MediaPlayer *player);
  bool is_ota_active() const;
  template<typename T> void set_uid_global(T *uid) {
    this->uid_reader_ = [uid]() -> std::string {
      if (uid == nullptr) return "";
      return uid->value();
    };
  }
  std::string get_uid() const {
    if (this->uid_reader_) return this->uid_reader_();
    return this->cached_uid_;
  }

 protected:
  int async_core_{0};
  std::string cached_uid_{"0000"};
  std::function<std::string()> uid_reader_{};
};

template<typename... Ts> class LuaRunFileAction : public Action<Ts...> {
 public:
  explicit LuaRunFileAction(LuaRuntime *parent) : parent_(parent) {}

  TEMPLATABLE_VALUE(std::string, path)

  void play(Ts... x) override { this->parent_->run_file(this->path_.value(x...)); }

 protected:
  LuaRuntime *parent_;
};

template<typename... Ts> class LuaRunFileAsyncAction : public Action<Ts...> {
 public:
  explicit LuaRunFileAsyncAction(LuaRuntime *parent) : parent_(parent) {}

  TEMPLATABLE_VALUE(std::string, path)

  void play(Ts... x) override { this->parent_->run_file_async(this->path_.value(x...)); }

 protected:
  LuaRuntime *parent_;
};

}  // namespace lua_runtime
}  // namespace esphome



