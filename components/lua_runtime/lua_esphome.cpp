#include "lua_esphome.h"

#ifdef LUA_RUNTIME_STUB
struct lua_State;

namespace esphome {
namespace lua_runtime {

void register_esphome_api(lua_State *) {}

}  // namespace lua_runtime
}  // namespace esphome

#else

#include "esphome/components/switch/switch.h"
#include "esphome/core/application.h"

#include "lua.hpp"

namespace esphome {
namespace lua_runtime {

static int lua_switch_get_state(lua_State *L) {
  const char *identifier = luaL_checkstring(L, 1);

#ifdef USE_SWITCH
  for (auto *sw : App.get_switches()) {
    if (sw == nullptr) continue;
    if (sw->get_object_id() == identifier || sw->get_name() == identifier) {
      lua_pushboolean(L, sw->state);
      return 1;
    }
  }

  lua_pushnil(L);
  lua_pushfstring(L, "switch not found: %s", identifier);
  return 2;
#else
  lua_pushnil(L);
  lua_pushstring(L, "switch support is not enabled");
  return 2;
#endif
}

static void register_switch_api(lua_State *L) {
  lua_newtable(L);  // switch

  lua_pushcfunction(L, lua_switch_get_state);
  lua_setfield(L, -2, "get_state");

  lua_setglobal(L, "switch");
}

void register_esphome_api(lua_State *L) { register_switch_api(L); }

}  // namespace lua_runtime
}  // namespace esphome

#endif
