#include "lua_runtime.h"

#include <cctype>
#include <fstream>
#include <streambuf>

#include "esphome/core/log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "lua_rtos.h"


#ifdef LUA_RUNTIME_STUB
// Stub mode: no Lua linked. Build succeeds but run_file always fails.
#else
#include "lua.hpp"
#endif

namespace esphome {
namespace lua_runtime {

static const char *TAG = "lua_runtime";
static const uint32_t LUA_TASK_STACK = 8192;
static const UBaseType_t LUA_TASK_PRIO = 1;

#ifndef LUA_RUNTIME_STUB
// Log level constants
static const int LOG_SILENT = 0;
static const int LOG_DEBUG = 1;
static const int LOG_INFO = 2;
static const int LOG_WARN = 3;
static const int LOG_ERROR = 4;
static const int LOG_FATAL = 5;

static int g_log_level = LOG_INFO;
static int g_log_style = 0;

static std::string to_upper(std::string s) {
  for (auto &ch : s) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
  return s;
}

static const char *level_to_letter(int level) {
  switch (level) {
    case LOG_DEBUG:
      return "D";
    case LOG_INFO:
      return "I";
    case LOG_WARN:
      return "W";
    case LOG_ERROR:
      return "E";
    case LOG_FATAL:
      return "F";
    default:
      return "?";
  }
}

static bool should_log(int level) { return level >= g_log_level && g_log_level != LOG_SILENT; }

static int lua_log_set_level(lua_State *L) {
  int level = LOG_INFO;
  if (lua_isstring(L, 1)) {
    std::string s = lua_tostring(L, 1);
    s = to_upper(s);
    if (s == "SILENT") level = LOG_SILENT;
    else if (s == "DEBUG") level = LOG_DEBUG;
    else if (s == "INFO") level = LOG_INFO;
    else if (s == "WARN") level = LOG_WARN;
    else if (s == "ERROR") level = LOG_ERROR;
    else if (s == "FATAL") level = LOG_FATAL;
  } else if (lua_isnumber(L, 1)) {
    level = static_cast<int>(lua_tointeger(L, 1));
  }
  if (level < LOG_SILENT) level = LOG_SILENT;
  if (level > LOG_FATAL) level = LOG_FATAL;
  g_log_level = level;
  return 0;
}

static int lua_log_get_level(lua_State *L) {
  lua_pushinteger(L, g_log_level);
  return 1;
}

static int lua_log_style(lua_State *L) {
  if (lua_gettop(L) == 0) {
    lua_pushinteger(L, g_log_style);
    return 1;
  }
  int style = static_cast<int>(luaL_checkinteger(L, 1));
  if (style < 0) style = 0;
  if (style > 2) style = 2;
  g_log_style = style;
  lua_pushinteger(L, g_log_style);
  return 1;
}

static void append_args(lua_State *L, int start, std::string &out) {
  int n = lua_gettop(L);
  for (int i = start; i <= n; i++) {
    size_t len = 0;
    const char *s = luaL_tolstring(L, i, &len);
    if (s != nullptr && len > 0) {
      if (!out.empty()) out += " ";
      out.append(s, len);
    }
    lua_pop(L, 1);
  }
}

static std::string get_where(lua_State *L) {
  luaL_where(L, 2);
  const char *w = lua_tostring(L, -1);
  std::string where = w ? w : "";
  lua_pop(L, 1);
  // trim trailing ": " added by luaL_where
  if (where.size() >= 2 && where.rfind(": ") == where.size() - 2) {
    where = where.substr(0, where.size() - 2);
  }
  return where;
}

static int lua_log_print(lua_State *L, int level) {
  if (!should_log(level)) return 0;
  const char *tag = luaL_checkstring(L, 1);
  std::string msg;
  append_args(L, 2, msg);

  std::string where = (g_log_style == 0) ? std::string() : get_where(L);

  std::string line;
  if (g_log_style == 0) {
    line = std::string(level_to_letter(level)) + "/" + tag + " " + msg;
  } else if (g_log_style == 1) {
    line = std::string(level_to_letter(level)) + "/" + where + " " + msg;
  } else {
    line = std::string(level_to_letter(level)) + "/" + tag + " " + where + " " + msg;
  }

  ESP_LOGI(TAG, "%s", line.c_str());
  return 0;
}

static int lua_log_debug(lua_State *L) { return lua_log_print(L, LOG_DEBUG); }
static int lua_log_info(lua_State *L) { return lua_log_print(L, LOG_INFO); }
static int lua_log_warn(lua_State *L) { return lua_log_print(L, LOG_WARN); }
static int lua_log_error(lua_State *L) { return lua_log_print(L, LOG_ERROR); }

static int lua_log_call(lua_State *L) { return lua_log_print(L, LOG_INFO); }

static int lua_delay_ms(lua_State *L) {
  int ms = (int) luaL_checkinteger(L, 1);
  if (ms < 0) ms = 0;
  vTaskDelay(pdMS_TO_TICKS(ms));
  return 0;
}

static void set_package_path(lua_State *L, const std::string &script_path) {
  std::string dir = script_path;
  size_t pos = dir.find_last_of('/');
  if (pos == std::string::npos) {
    dir = ".";
  } else {
    dir = dir.substr(0, pos);
  }

  lua_getglobal(L, "package");
  if (!lua_istable(L, -1)) {
    lua_pop(L, 1);
    return;
  }
  lua_getfield(L, -1, "path");
  const char *old_path = lua_tostring(L, -1);
  std::string new_path;
  if (old_path != nullptr) new_path = old_path;

  // Prepend app directory search paths so `require \"sys\"` resolves to ./sys.lua
  std::string prefix = dir + "/?.lua;" + dir + "/?/init.lua;" + dir + "/lib/?.lua;";
  new_path = prefix + new_path;

  lua_pop(L, 1);  // pop old path
  lua_pushstring(L, new_path.c_str());
  lua_setfield(L, -2, "path");
  lua_pop(L, 1);  // pop package
}

static void register_log_api(lua_State *L) {
  lua_newtable(L);  // log

  // constants
  lua_pushinteger(L, LOG_SILENT);
  lua_setfield(L, -2, "LOG_SILENT");
  lua_pushinteger(L, LOG_DEBUG);
  lua_setfield(L, -2, "LOG_DEBUG");
  lua_pushinteger(L, LOG_INFO);
  lua_setfield(L, -2, "LOG_INFO");
  lua_pushinteger(L, LOG_WARN);
  lua_setfield(L, -2, "LOG_WARN");
  lua_pushinteger(L, LOG_ERROR);
  lua_setfield(L, -2, "LOG_ERROR");
  lua_pushinteger(L, LOG_FATAL);
  lua_setfield(L, -2, "LOG_FATAL");

  // functions
  lua_pushcfunction(L, lua_log_set_level);
  lua_setfield(L, -2, "setLevel");
  lua_pushcfunction(L, lua_log_get_level);
  lua_setfield(L, -2, "getLevel");
  lua_pushcfunction(L, lua_log_style);
  lua_setfield(L, -2, "style");
  lua_pushcfunction(L, lua_log_debug);
  lua_setfield(L, -2, "debug");
  lua_pushcfunction(L, lua_log_info);
  lua_setfield(L, -2, "info");
  lua_pushcfunction(L, lua_log_warn);
  lua_setfield(L, -2, "warn");
  lua_pushcfunction(L, lua_log_error);
  lua_setfield(L, -2, "error");

  // metatable to allow log("tag", ...) as info
  lua_newtable(L);
  lua_pushcfunction(L, lua_log_call);
  lua_setfield(L, -2, "__call");
  lua_setmetatable(L, -2);

  lua_setglobal(L, "log");
}


static void register_base_api(lua_State *L) {
  register_log_api(L);
  register_rtos_api(L);

  lua_pushcfunction(L, lua_delay_ms);
  lua_setglobal(L, "delay_ms");
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

void LuaRuntime::mark_task_done() { this->running_.store(false); }

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

  RtosContext ctx;
  rtos_init(ctx);

  luaL_openlibs(L);
  register_base_api(L);
  set_package_path(L, path);

  int load_status = luaL_loadbuffer(L, script.data(), script.size(), path.c_str());
  if (load_status != LUA_OK) {
    const char *err = lua_tostring(L, -1);
    ESP_LOGE(TAG, "Lua load error: %s", err ? err : "(unknown)");
    lua_close(L);
    rtos_cleanup(ctx);
    return false;
  }

  int call_status = lua_pcall(L, 0, LUA_MULTRET, 0);
  if (call_status != LUA_OK) {
    const char *err = lua_tostring(L, -1);
    ESP_LOGE(TAG, "Lua runtime error: %s", err ? err : "(unknown)");
    lua_close(L);
    rtos_cleanup(ctx);
    return false;
  }

  lua_close(L);
  rtos_cleanup(ctx);
  g_ctx = nullptr;
  ESP_LOGI(TAG, "Lua script finished: %s", path.c_str());
  return true;
#endif
}

struct LuaTaskArgs {
  LuaRuntime *self;
  std::string path;
};

static void lua_task_entry(void *param) {
  auto *args = static_cast<LuaTaskArgs *>(param);
  LuaRuntime *self = args->self;
  std::string path = args->path;
  delete args;

  self->run_file(path);
  self->mark_task_done();
  vTaskDelete(nullptr);
}

bool LuaRuntime::run_file_async(const std::string &path) {
#ifdef LUA_RUNTIME_STUB
  ESP_LOGE(TAG, "Lua runtime is in stub mode. Add Lua sources and disable enable_stub.");
  ESP_LOGE(TAG, "Requested: %s", path.c_str());
  return false;
#else
  bool expected = false;
  if (!this->running_.compare_exchange_strong(expected, true)) {
    ESP_LOGW(TAG, "Lua task already running, skip: %s", path.c_str());
    return false;
  }

  auto *args = new LuaTaskArgs{this, path};
  BaseType_t ok = xTaskCreatePinnedToCore(
      lua_task_entry, "lua_task", LUA_TASK_STACK, args, LUA_TASK_PRIO, nullptr, 1);
  if (ok != pdPASS) {
    ESP_LOGE(TAG, "Failed to create Lua task");
    delete args;
    this->running_.store(false);
    return false;
  }
  return true;
#endif
}

}  // namespace lua_runtime
}  // namespace esphome
