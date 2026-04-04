#include "lua_runtime.h"

#include <cctype>
#include <fstream>
#include <streambuf>
#include <unordered_map>

#include "esphome/core/log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_timer.h"

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

// RTOS constants
static const int MSG_TIMER = 0x100;
static const int INF_TIMEOUT = -1;

struct RtosMsg {
  int msgid;
  int id;
  int repeat;
};

struct RtosTimer {
  int id;
  int repeat;
  uint32_t timeout_ms;
  esp_timer_handle_t handle;
};

struct RtosContext {
  QueueHandle_t queue{nullptr};
  SemaphoreHandle_t lock{nullptr};
  std::unordered_map<int, RtosTimer *> timers;
  uint32_t autogc_period{100};
  uint32_t autogc_mid{80};
  uint32_t autogc_high{90};
  uint32_t autogc_counter{0};
};

static RtosContext *g_ctx = nullptr;
static size_t g_lua_max_used = 0;

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

  // Search order (high -> low):
  // 1) script directory
  // 2) script directory /lib
  // 3) shared /sdcard/lib
  std::string prefix = dir + "/?.lua;" + dir + "/lib/?.lua;" + "/sdcard/lib/?.lua;";
  new_path = prefix + new_path;

  lua_pop(L, 1);  // pop old path
  lua_pushstring(L, new_path.c_str());
  lua_setfield(L, -2, "path");
  lua_pop(L, 1);  // pop package
}

static void timer_cleanup(RtosTimer *timer) {
  if (timer == nullptr) return;
  if (timer->handle != nullptr) {
    esp_timer_stop(timer->handle);
    esp_timer_delete(timer->handle);
  }
  delete timer;
}

static void rtos_timer_cb(void *arg) {
  auto *timer = static_cast<RtosTimer *>(arg);
  if (timer == nullptr || g_ctx == nullptr || g_ctx->queue == nullptr) return;

  RtosMsg msg{MSG_TIMER, timer->id, timer->repeat};
  xQueueSend(g_ctx->queue, &msg, 0);

  if (timer->repeat == 0) {
    if (g_ctx->lock) xSemaphoreTake(g_ctx->lock, portMAX_DELAY);
    g_ctx->timers.erase(timer->id);
    if (g_ctx->lock) xSemaphoreGive(g_ctx->lock);
    timer_cleanup(timer);
  } else if (timer->repeat > 0) {
    timer->repeat--;
    if (timer->repeat == 0) {
      if (g_ctx->lock) xSemaphoreTake(g_ctx->lock, portMAX_DELAY);
      g_ctx->timers.erase(timer->id);
      if (g_ctx->lock) xSemaphoreGive(g_ctx->lock);
      timer_cleanup(timer);
    }
  }
}

static int rtos_receive(lua_State *L) {
  if (g_ctx == nullptr || g_ctx->queue == nullptr) {
    lua_pushinteger(L, -1);
    return 1;
  }

  int timeout = (int) luaL_optinteger(L, 1, -1);
  TickType_t ticks = (timeout < 0) ? portMAX_DELAY : pdMS_TO_TICKS(timeout);

  RtosMsg msg{0, 0, 0};
  if (xQueueReceive(g_ctx->queue, &msg, ticks) != pdTRUE) {
    lua_pushinteger(L, -1);
    return 1;
  }


  // auto GC policy: run on receive boundary
  if (g_ctx->autogc_period > 0) {
    g_ctx->autogc_counter++;
    if (g_ctx->autogc_counter >= g_ctx->autogc_period) {
      g_ctx->autogc_counter = 0;
      size_t total = (size_t) lua_gc(L, LUA_GCCOUNT, 0) * 1024;
      size_t used = total;
      if (total > 0 && (used * 100) >= (total * g_ctx->autogc_high)) {
        lua_gc(L, LUA_GCCOLLECT, 0);
        lua_gc(L, LUA_GCCOLLECT, 0);
      } else if (total > 0 && (used * 100) >= (total * g_ctx->autogc_mid)) {
        lua_gc(L, LUA_GCCOLLECT, 0);
        lua_gc(L, LUA_GCCOLLECT, 0);
      }
    }
  }

  lua_pushinteger(L, msg.msgid);
  lua_pushinteger(L, msg.id);
  lua_pushinteger(L, msg.repeat);
  return 3;
}

static int rtos_timer_start(lua_State *L) {
  if (g_ctx == nullptr) {
    ESP_LOGE(TAG, "rtos.timer_start: no context");
    lua_pushinteger(L, 0);
    return 1;
  }
  if (g_ctx->queue == nullptr) {
    ESP_LOGE(TAG, "rtos.timer_start: queue null");
  }
  if (g_ctx->lock == nullptr) {
    ESP_LOGE(TAG, "rtos.timer_start: lock null");
  }
  int id = (int) luaL_checkinteger(L, 1);
  int timeout = (int) luaL_checkinteger(L, 2);
  int repeat = (int) luaL_optinteger(L, 3, 0);
  if (timeout < 1) {
    lua_pushinteger(L, 0);
    return 1;
  }

  if (g_ctx->lock) xSemaphoreTake(g_ctx->lock, portMAX_DELAY);
  auto it = g_ctx->timers.find(id);
  if (it != g_ctx->timers.end()) {
    timer_cleanup(it->second);
    g_ctx->timers.erase(it);
  }
  if (g_ctx->lock) xSemaphoreGive(g_ctx->lock);

  auto *timer = new RtosTimer{ id, repeat, (uint32_t) timeout, nullptr };

  esp_timer_create_args_t args{};
  args.callback = &rtos_timer_cb;
  args.arg = timer;
  args.dispatch_method = ESP_TIMER_TASK;
  args.name = "lua_rtos";

  esp_err_t err = esp_timer_create(&args, &timer->handle);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "rtos.timer_start create fail: %s", esp_err_to_name(err));
    delete timer;
    lua_pushinteger(L, 0);
    return 1;
  }

  if (g_ctx->lock) xSemaphoreTake(g_ctx->lock, portMAX_DELAY);
  g_ctx->timers[id] = timer;
  if (g_ctx->lock) xSemaphoreGive(g_ctx->lock);

  if (repeat == 0) {
    err = esp_timer_start_once(timer->handle, (uint64_t) timeout * 1000);
  } else {
    err = esp_timer_start_periodic(timer->handle, (uint64_t) timeout * 1000);
  }

  if (err != ESP_OK) {
    ESP_LOGW(TAG, "rtos.timer_start start fail: %s", esp_err_to_name(err));
    if (g_ctx->lock) xSemaphoreTake(g_ctx->lock, portMAX_DELAY);
    g_ctx->timers.erase(id);
    if (g_ctx->lock) xSemaphoreGive(g_ctx->lock);
    timer_cleanup(timer);
    lua_pushinteger(L, 0);
    return 1;
  }

  lua_pushinteger(L, 1);
  return 1;
}

static int rtos_timer_stop(lua_State *L) {
  if (g_ctx == nullptr) return 0;
  if (!lua_isinteger(L, 1)) return 0;
  int id = (int) lua_tointeger(L, 1);

  if (g_ctx->lock) xSemaphoreTake(g_ctx->lock, portMAX_DELAY);
  auto it = g_ctx->timers.find(id);
  if (it != g_ctx->timers.end()) {
    auto *timer = it->second;
    g_ctx->timers.erase(it);
    if (g_ctx->lock) xSemaphoreGive(g_ctx->lock);
    timer_cleanup(timer);
    return 0;
  }
  if (g_ctx->lock) xSemaphoreGive(g_ctx->lock);
  return 0;
}

static int rtos_reboot(lua_State *L) {
  (void) L;
  ESP_LOGW(TAG, "Script requested reboot but rejected.");
  return 0;
}

static int rtos_build_date(lua_State *L) {
  lua_pushstring(L, __DATE__);
  return 1;
}

static int rtos_bsp(lua_State *L) {
#if CONFIG_IDF_TARGET_ESP32S3
  lua_pushstring(L, "ESP32S3");
#elif CONFIG_IDF_TARGET_ESP32
  lua_pushstring(L, "ESP32");
#elif CONFIG_IDF_TARGET_ESP32C3
  lua_pushstring(L, "ESP32C3");
#else
  lua_pushstring(L, "ESP32");
#endif
  return 1;
}

static const char *get_lemonade_version() {
#ifdef LEMONADE_VERSION
  return LEMONADE_VERSION;
#else
  return ESPHOME_VERSION;
#endif
}

static int rtos_version(lua_State *L) {
  lua_pushstring(L, get_lemonade_version());
  if (lua_isboolean(L, 1) && lua_toboolean(L, 1)) {
    lua_pushinteger(L, 0);  // numeric version not defined
    lua_pushinteger(L, 32);
    return 3;
  }
  return 1;
}

static int rtos_standy(lua_State *L) {
  int timeout = (int) luaL_optinteger(L, 1, 0);
  if (timeout > 0) vTaskDelay(pdMS_TO_TICKS(timeout));
  return 0;
}

static int rtos_meminfo(lua_State *L) {
  const char *type = luaL_optstring(L, 1, "lua");

  size_t total = 0;
  size_t free = 0;
  size_t used = 0;
  size_t max_used = 0;

  if (strcmp(type, "psram") == 0) {
    total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  } else {
    total = heap_caps_get_total_size(MALLOC_CAP_DEFAULT);
    free = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
  }
  used = (total > free) ? (total - free) : 0;

  if (strcmp(type, "lua") == 0) {
    size_t lua_kb = (size_t) lua_gc(L, LUA_GCCOUNT, 0);
    size_t lua_b = (size_t) lua_gc(L, LUA_GCCOUNTB, 0);
    used = lua_kb * 1024 + lua_b;
    if (used > g_lua_max_used) g_lua_max_used = used;
    max_used = g_lua_max_used;
  } else {
    max_used = used;
  }

  lua_pushinteger(L, (lua_Integer) total);
  lua_pushinteger(L, (lua_Integer) used);
  lua_pushinteger(L, (lua_Integer) max_used);
  return 3;
}

static int rtos_firmware(lua_State *L) {
#if CONFIG_IDF_TARGET_ESP32S3
  const char *bsp = "ESP32S3";
#elif CONFIG_IDF_TARGET_ESP32
  const char *bsp = "ESP32";
#elif CONFIG_IDF_TARGET_ESP32C3
  const char *bsp = "ESP32C3";
#else
  const char *bsp = "ESP32";
#endif
  lua_pushfstring(L, "LemonadeOS_%s_%s", get_lemonade_version(), bsp);
  return 1;
}

static std::string replace_all(std::string s, const std::string &from, const std::string &to) {
  size_t pos = 0;
  while ((pos = s.find(from, pos)) != std::string::npos) {
    s.replace(pos, from.length(), to);
    pos += to.length();
  }
  return s;
}

static int rtos_set_paths(lua_State *L) {
  lua_getglobal(L, "package");
  if (!lua_istable(L, -1)) {
    lua_pop(L, 1);
    return 0;
  }

  std::string prefix;
  for (int i = 1; i <= 4; i++) {
    if (lua_isstring(L, i)) {
      std::string p = lua_tostring(L, i);
      if (!p.empty()) {
        p = replace_all(p, "%s", "?");
        if (p.find("?") == std::string::npos) {
          // ensure it's a module pattern
          if (!p.empty() && p.back() != '/') p += '/';
          p += "?.lua";
        }
        prefix += p + ";";
      }
    }
  }

  lua_getfield(L, -1, "path");
  const char *old_path = lua_tostring(L, -1);
  std::string new_path = prefix + (old_path ? old_path : "");
  lua_pop(L, 1);

  lua_pushstring(L, new_path.c_str());
  lua_setfield(L, -2, "path");
  lua_pop(L, 1);  // pop package
  return 0;
}

static int rtos_nop(lua_State *L) {
  (void) L;
  return 0;
}

static int rtos_auto_collect_mem(lua_State *L) {
  if (g_ctx == nullptr) return 0;
  uint32_t period = (uint32_t) luaL_optinteger(L, 1, 100);
  uint32_t mid = (uint32_t) luaL_optinteger(L, 2, 80);
  uint32_t high = (uint32_t) luaL_optinteger(L, 3, 90);
  if (period > 60000) return 0;
  if (mid > 95 || high > 95) return 0;
  if (mid < 50 || high < 50) return 0;
  if (mid >= high) return 0;
  g_ctx->autogc_period = period;
  g_ctx->autogc_mid = mid;
  g_ctx->autogc_high = high;
  g_ctx->autogc_counter = 0;
  return 0;
}

static void rtos_cleanup(RtosContext &ctx) {
  if (ctx.lock) xSemaphoreTake(ctx.lock, portMAX_DELAY);
  for (auto &kv : ctx.timers) {
    timer_cleanup(kv.second);
  }
  ctx.timers.clear();
  if (ctx.lock) xSemaphoreGive(ctx.lock);

  if (ctx.queue) {
    vQueueDelete(ctx.queue);
    ctx.queue = nullptr;
  }
  if (ctx.lock) {
    vSemaphoreDelete(ctx.lock);
    ctx.lock = nullptr;
  }
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

static void register_rtos_api(lua_State *L) {
  lua_newtable(L);  // rtos

  lua_pushcfunction(L, rtos_receive);
  lua_setfield(L, -2, "receive");
  lua_pushcfunction(L, rtos_timer_start);
  lua_setfield(L, -2, "timer_start");
  lua_pushcfunction(L, rtos_timer_stop);
  lua_setfield(L, -2, "timer_stop");
  lua_pushcfunction(L, rtos_reboot);
  lua_setfield(L, -2, "reboot");
  lua_pushcfunction(L, rtos_build_date);
  lua_setfield(L, -2, "buildDate");
  lua_pushcfunction(L, rtos_bsp);
  lua_setfield(L, -2, "bsp");
  lua_pushcfunction(L, rtos_version);
  lua_setfield(L, -2, "version");
  lua_pushcfunction(L, rtos_standy);
  lua_setfield(L, -2, "standy");
  lua_pushcfunction(L, rtos_meminfo);
  lua_setfield(L, -2, "meminfo");
  lua_pushcfunction(L, rtos_firmware);
  lua_setfield(L, -2, "firmware");
  lua_pushcfunction(L, rtos_set_paths);
  lua_setfield(L, -2, "setPaths");
  lua_pushcfunction(L, rtos_nop);
  lua_setfield(L, -2, "nop");
  lua_pushcfunction(L, rtos_auto_collect_mem);
  lua_setfield(L, -2, "autoCollectMem");

  // constants
  lua_pushinteger(L, INF_TIMEOUT);
  lua_setfield(L, -2, "INF_TIMEOUT");
  lua_pushinteger(L, MSG_TIMER);
  lua_setfield(L, -2, "MSG_TIMER");

  lua_setglobal(L, "rtos");
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
  ctx.queue = xQueueCreate(16, sizeof(RtosMsg));
  ctx.lock = xSemaphoreCreateMutex();
  g_ctx = &ctx;

  if (ctx.queue == nullptr) {
    ESP_LOGE(TAG, "rtos.queue create failed");
  }
  if (ctx.lock == nullptr) {
    ESP_LOGE(TAG, "rtos.lock create failed");
  }

  esp_err_t init_err = esp_timer_init();
  if (init_err != ESP_OK && init_err != ESP_ERR_INVALID_STATE) {
    ESP_LOGW(TAG, "rtos.timer_start init fail: %s", esp_err_to_name(init_err));
  }

  luaL_openlibs(L);
  register_base_api(L);
  set_package_path(L, path);

  int load_status = luaL_loadbuffer(L, script.data(), script.size(), path.c_str());
  if (load_status != LUA_OK) {
    const char *err = lua_tostring(L, -1);
    ESP_LOGE(TAG, "Lua load error: %s", err ? err : "(unknown)");
    lua_close(L);
    rtos_cleanup(ctx);
    g_ctx = nullptr;
    return false;
  }

  int call_status = lua_pcall(L, 0, LUA_MULTRET, 0);
  if (call_status != LUA_OK) {
    const char *err = lua_tostring(L, -1);
    ESP_LOGE(TAG, "Lua runtime error: %s", err ? err : "(unknown)");
    lua_close(L);
    rtos_cleanup(ctx);
    g_ctx = nullptr;
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
