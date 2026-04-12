#pragma once

struct lua_State;

namespace esphome {
namespace lua_runtime {

void register_esphome_api(lua_State *L);

}  // namespace lua_runtime
}  // namespace esphome
