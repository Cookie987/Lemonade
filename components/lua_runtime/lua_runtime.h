#pragma once

#include <atomic>
#include <string>

#include "esphome/core/automation.h"
#include "esphome/core/component.h"

namespace esphome {
namespace lua_runtime {

class LuaRuntime : public Component {
 public:
  void setup() override;
  void dump_config() override;
  bool run_file(const std::string &path);
  bool run_file_async(const std::string &path);
  void mark_task_done();

 protected:
  std::atomic<bool> running_{false};
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
