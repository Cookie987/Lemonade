#pragma once

#include <functional>
#include <string>
#include <utility>
#include <vector>

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
namespace usb_cdc_rx {
class USBCdcRx;
}  // namespace usb_cdc_rx
}  // namespace esphome

namespace esphome {
namespace lua_runtime {

bool is_active_app_page();
void lua_abort_if_ota(lua_State *L);
// Record this state's Lua heap / stack usage (must be called from the task that owns L).
void lua_sample_self_memory(lua_State *L);
// True when this state should stop (per-app stop or runtime abort_all).
bool lua_abort_requested(lua_State *L);

// One running Lua app, for the settings "running apps" page.
struct LuaAppInfo {
  std::string dir;      // /sdcard/opt/com.lemonade.dictionary
  std::string name;     // manifest.json name, falls back to the directory name
  std::string script;   // .../main.lua
  uint32_t lua_bytes{0};    // Lua heap of this state, sampled by its own task
  uint32_t stack_bytes{0};  // peak stack usage of the Lua task
  uint32_t running_ms{0};   // wall clock since the script started
  bool parked{false};       // suspended in the background (state kept)
  bool aborting{false};     // stop requested, task is winding down
};

// Device-wide memory snapshot for the same page.
struct LuaMemoryInfo {
  uint32_t psram_total{0};
  uint32_t psram_free{0};
  uint32_t psram_largest{0};
  uint32_t internal_total{0};
  uint32_t internal_free{0};
  uint32_t internal_largest{0};
  uint32_t lua_bytes_total{0};
};

// Async HTTP plumbing shared between lua_runtime.cpp and lua_httplib.cpp.
std::string lua_runtime_script_path(lua_State *L);
void lua_http_dispatch(lua_State *L, const std::string &owner);
void lua_http_cancel_owner(lua_State *L, const std::string &owner);
void lua_http_notify(const std::string &owner);

class LuaRuntime : public Component {
 public:
  void setup() override;
  void dump_config() override;
  void loop() override;
  bool run_file(const std::string &path);
  bool run_file_async(const std::string &path);
  void mark_task_done(const std::string &path);
  void abort_all();
  /// Snapshot of the running Lua apps (settings page). Safe to call from the main task.
  std::vector<LuaAppInfo> list_apps();
  /// Stops one app by directory (e.g. "/sdcard/opt/com.lemonade.dictionary").
  bool stop_app(const std::string &app_dir);
  /// PSRAM/internal RAM snapshot plus the total Lua heap of all running apps.
  LuaMemoryInfo memory_info();
  void set_async_core(int async_core) { this->async_core_ = async_core; }
  void set_ota_active(bool active);
  /// Gate for the idle-app lifecycle: when it returns false (locked / asleep) background
  /// apps are not stopped.
  void set_lifecycle_gate(std::function<bool()> gate) { this->lifecycle_gate_ = std::move(gate); }
  void register_rtttl_player(const std::string &name, rtttl::Rtttl *player);
  void register_media_player(const std::string &name, media_player::MediaPlayer *player);
  /// USB CDC bridge for the Lua `usb` API (see lua_usb.cpp). Optional.
  void set_usb_source(usb_cdc_rx::USBCdcRx *source) { this->usb_source_ = source; }
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
  void update_app_lifecycle();

  int async_core_{0};
  std::function<bool()> lifecycle_gate_{};
  std::string cached_uid_{"0000"};
  std::function<std::string()> uid_reader_{};
  usb_cdc_rx::USBCdcRx *usb_source_{nullptr};
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



