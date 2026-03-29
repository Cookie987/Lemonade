#include "lua_runtime.h"

#include <fstream>
#include <streambuf>

#include "esphome/core/log.h"

#ifdef LUA_RUNTIME_STUB
// Stub mode: no Lua linked. Build succeeds but run_file always fails.
#else
#include "lua.hpp"
#endif

namespace esphome {
namespace lua_runtime {

static const char *TAG = "lua_runtime";

#ifndef LUA_RUNTIME_STUB
static int lua_log(lua_State *L) {
  int n = lua_gettop(L);
  std::string msg;
  for (int i = 1; i <= n; i++) {
    size_t len = 0;
    const char *s = luaL_tolstring(L, i, &len);  // pushes a string on stack
    if (s != nullptr && len > 0) {
      if (!msg.empty()) msg += " ";
      msg.append(s, len);
    }
    lua_pop(L, 1);  // pop result of luaL_tolstring
  }
  ESP_LOGI(TAG, "%s", msg.c_str());
  return 0;
}

static void register_base_api(lua_State *L) {
  lua_pushcfunction(L, lua_log);
  lua_setglobal(L, "log");
}
#endif

void LuaRuntime::setup() {}

void LuaRuntime::dump_config() {
  ESP_LOGCONFIG(TAG, "Lua Runtime");
#ifdef LUA_RUNTIME_STUB
  ESP_LOGCONFIG(TAG, "  Mode: stub (no Lua linked)");
#else
  ESP_LOGCONFIG(TAG, "  Mode: enabled");
#endif
}

bool LuaRuntime::run_file(const std::string &path) {
#ifdef LUA_RUNTIME_STUB
  ESP_LOGE(TAG, "Lua runtime is in stub mode. Add Lua sources and disable enable_stub.");
  ESP_LOGE(TAG, "Requested: %s", path.c_str());
  return false;
#else
  std::ifstream file(path, std::ios::in | std::ios::binary);
  if (!file.is_open()) {
    ESP_LOGE(TAG, "Failed to open Lua file: %s", path.c_str());
    return false;
  }

  std::string script((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  file.close();

  lua_State *L = luaL_newstate();
  if (L == nullptr) {
    ESP_LOGE(TAG, "Failed to create Lua state");
    return false;
  }

  luaL_openlibs(L);
  register_base_api(L);

  int load_status = luaL_loadbuffer(L, script.data(), script.size(), path.c_str());
  if (load_status != LUA_OK) {
    const char *err = lua_tostring(L, -1);
    ESP_LOGE(TAG, "Lua load error: %s", err ? err : "(unknown)");
    lua_close(L);
    return false;
  }

  int call_status = lua_pcall(L, 0, LUA_MULTRET, 0);
  if (call_status != LUA_OK) {
    const char *err = lua_tostring(L, -1);
    ESP_LOGE(TAG, "Lua runtime error: %s", err ? err : "(unknown)");
    lua_close(L);
    return false;
  }

  lua_close(L);
  ESP_LOGI(TAG, "Lua script finished: %s", path.c_str());
  return true;
#endif
}

}  // namespace lua_runtime
}  // namespace esphome
