#pragma once

#include <string>

struct lua_State;

namespace esphome {
namespace lua_runtime {

void register_fskv_api(lua_State *L, const std::string &script_path);

}  // namespace lua_runtime
}  // namespace esphome
