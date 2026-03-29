#pragma once

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
};

template<typename... Ts> class LuaRunFileAction : public Action<Ts...> {
 public:
  explicit LuaRunFileAction(LuaRuntime *parent) : parent_(parent) {}

  TEMPLATABLE_VALUE(std::string, path)

  void play(Ts... x) override { this->parent_->run_file(this->path_.value(x...)); }

 protected:
  LuaRuntime *parent_;
};

}  // namespace lua_runtime
}  // namespace esphome
