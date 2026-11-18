#include "lua_runtime.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "esphome/core/log.h"
#include "esphome/core/preferences.h"
#include "lua_app.h"
#include "lua_fskv.h"
#include "lua_esphome.h"
#include "lua_fs.h"
#include "lua_lvgl.h"
#include "lua_usb.h"

#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_heap_caps.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"

#ifdef LUA_RUNTIME_STUB
// Stub mode: no Lua linked. Build succeeds but run_file always fails.
#else
#include "lua.hpp"
extern "C" int luaopen_json(lua_State *L);
extern "C" int luaopen_http(lua_State *L);
#endif

namespace esphome {
namespace lua_runtime {

static const char *TAG = "lua_runtime";
static const uint32_t LUA_TASK_STACK = 20480;  // HTTPS/TLS + JSON needs headroom (was 8192)
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
static const int MSG_HTTP = 0x101;
static const int INF_TIMEOUT = -1;

struct RtosMsg {
  int msgid;
  int id;
  int repeat;
};

struct RtosContext;

struct RtosTimer {
  RtosContext *ctx;
  int id;
  int repeat;
  uint32_t timeout_ms;
  esp_timer_handle_t handle;
  bool paused{false};
};

struct RtosContext {
  QueueHandle_t queue{nullptr};
  SemaphoreHandle_t lock{nullptr};
  std::unordered_map<int, RtosTimer *> timers;
  uint32_t autogc_period{100};
  uint32_t autogc_mid{80};
  uint32_t autogc_high{90};
  uint32_t autogc_counter{0};
  volatile bool alive{true};
  volatile bool abort_requested{false};
  volatile bool pause_requested{false};
  volatile bool parked{false};
  TaskHandle_t task{nullptr};
  uint32_t abort_generation{0};
  std::string path;
  // Memory sampling, written only by the task that owns this state (see rtos_receive).
  volatile uint32_t lua_bytes{0};
  volatile uint32_t stack_bytes{0};
  volatile uint32_t started_at{0};
  uint32_t sample_counter{0};
};

static SemaphoreHandle_t g_run_lock = nullptr;
static std::unordered_map<std::string, bool> g_run_paths;
// Paths whose relaunch was requested while the previous task was still aborting. They are
// started again as soon as the old task releases its run path, instead of being silently
// skipped ("already running") and leaving the launch cover up until the 3 s timeout.
static std::unordered_set<std::string> g_pending_restart;
// Lifecycle state, accessed from the LVGL owner task (loop) and from Lua tasks.
static SemaphoreHandle_t g_ctx_lock = nullptr;
static std::unordered_map<std::string, RtosContext *> g_ctx_by_path;
static std::unordered_map<std::string, uint32_t> g_app_background_since;
static std::unordered_set<std::string> g_app_pause_sent;
// Background apps are parked (task suspended, timers frozen) after this long and keep their
// memory. They are only stopped (Lua VM + widgets freed) when free PSRAM drops below
// APP_LOW_PSRAM_BYTES, and then only the oldest background app, one per loop.
static constexpr uint32_t APP_BACKGROUND_PAUSE_MS = 3000;
static constexpr size_t APP_LOW_PSRAM_BYTES = 1024 * 1024;
static const char *CTX_KEY = "lua_runtime.ctx";
static const char *ABORT_GEN_KEY = "lua_runtime.abort_generation";
static volatile bool g_ota_active = false;
static volatile uint32_t g_abort_generation = 0;
static constexpr uint32_t UID_NAME_HASH = 2557596578U;
static constexpr uint32_t GLOBAL_PREFERENCE_SEED = 1944399030U;
static constexpr size_t UID_RESTORE_SIZE = 64;

static bool ota_is_active() { return g_ota_active; }

static RtosContext *get_ctx(lua_State *L);
static void sample_ctx_memory(lua_State *L, RtosContext *ctx);

static void set_abort_generation(lua_State *L, uint32_t generation) {
  lua_pushlightuserdata(L, (void *) &ABORT_GEN_KEY);
  lua_pushinteger(L, generation);
  lua_settable(L, LUA_REGISTRYINDEX);
}

static uint32_t get_abort_generation(lua_State *L) {
  lua_pushlightuserdata(L, (void *) &ABORT_GEN_KEY);
  lua_gettable(L, LUA_REGISTRYINDEX);
  uint32_t generation = (uint32_t) lua_tointeger(L, -1);
  lua_pop(L, 1);
  return generation;
}

void lua_abort_if_ota(lua_State *L) {
  if (ota_is_active()) {
    luaL_error(L, "Lua script aborted because OTA is in progress");
  }
  if (get_abort_generation(L) != g_abort_generation) {
    luaL_error(L, "Lua script aborted because runtime abort was requested");
  }
}

void lua_sample_self_memory(lua_State *L) {
  RtosContext *ctx = get_ctx(L);
  if (ctx == nullptr) return;
  sample_ctx_memory(L, ctx);
}

bool lua_abort_requested(lua_State *L) {
  RtosContext *ctx = get_ctx(L);
  if (ctx == nullptr) return false;
  return ctx->abort_requested || ctx->abort_generation != g_abort_generation;
}

static void lua_ota_hook(lua_State *L, lua_Debug *ar) {
  (void) ar;
  lua_abort_if_ota(L);
}

static bool claim_run_path(const std::string &path) {
  if (g_run_lock == nullptr) {
    g_run_lock = xSemaphoreCreateMutex();
  }
  if (g_run_lock) xSemaphoreTake(g_run_lock, portMAX_DELAY);
  auto it = g_run_paths.find(path);
  if (it != g_run_paths.end() && it->second) {
    if (g_run_lock) xSemaphoreGive(g_run_lock);
    return false;
  }
  g_run_paths[path] = true;
  if (g_run_lock) xSemaphoreGive(g_run_lock);
  return true;
}

static void release_run_path(const std::string &path) {
  if (g_run_lock == nullptr) return;
  if (g_run_lock) xSemaphoreTake(g_run_lock, portMAX_DELAY);
  auto it = g_run_paths.find(path);
  if (it != g_run_paths.end()) {
    g_run_paths.erase(it);
  }
  if (g_run_lock) xSemaphoreGive(g_run_lock);
}

static void mark_pending_restart(const std::string &path) {
  if (g_run_lock == nullptr) return;
  xSemaphoreTake(g_run_lock, portMAX_DELAY);
  g_pending_restart.insert(path);
  xSemaphoreGive(g_run_lock);
}

static bool take_pending_restart(const std::string &path) {
  if (g_run_lock == nullptr) return false;
  bool pending = false;
  xSemaphoreTake(g_run_lock, portMAX_DELAY);
  auto it = g_pending_restart.find(path);
  if (it != g_pending_restart.end()) {
    g_pending_restart.erase(it);
    pending = true;
  }
  xSemaphoreGive(g_run_lock);
  return pending;
}
static void set_ctx(lua_State *L, RtosContext *ctx) {
  lua_pushlightuserdata(L, (void *) &CTX_KEY);
  lua_pushlightuserdata(L, ctx);
  lua_settable(L, LUA_REGISTRYINDEX);
}

static RtosContext *get_ctx(lua_State *L) {
  lua_pushlightuserdata(L, (void *) &CTX_KEY);
  lua_gettable(L, LUA_REGISTRYINDEX);
  auto *ctx = static_cast<RtosContext *>(lua_touserdata(L, -1));
  lua_pop(L, 1);
  return ctx;
}

// ---------------------------------------------------------------- app lifecycle
static void ensure_lifecycle_locks() {
  if (g_ctx_lock == nullptr) g_ctx_lock = xSemaphoreCreateMutex();
}

static std::string app_dir_of_script(const std::string &path) {
  size_t pos = path.find_last_of('/');
  if (pos == std::string::npos) return ".";
  return path.substr(0, pos);
}

static bool is_main_script(const std::string &path) {
  static const char *suffix = "/main.lua";
  const size_t n = 9;
  return path.size() >= n && path.compare(path.size() - n, n, suffix) == 0;
}

static void register_ctx(const std::string &path, RtosContext *ctx) {
  ensure_lifecycle_locks();
  if (g_ctx_lock == nullptr) return;
  xSemaphoreTake(g_ctx_lock, portMAX_DELAY);
  g_ctx_by_path[path] = ctx;
  xSemaphoreGive(g_ctx_lock);
}

// run_file calls this under the same lock right before its stack frame goes away, so any
// ctx found while holding g_ctx_lock is guaranteed to still be alive.
static void unregister_ctx(const std::string &path) {
  if (g_ctx_lock == nullptr) return;
  xSemaphoreTake(g_ctx_lock, portMAX_DELAY);
  g_ctx_by_path.erase(path);
  xSemaphoreGive(g_ctx_lock);
}

// Ask a background script to park itself at its next rtos.receive boundary.
static void pause_app_script(const std::string &path) {
  ensure_lifecycle_locks();
  if (g_ctx_lock == nullptr) return;
  xSemaphoreTake(g_ctx_lock, portMAX_DELAY);
  auto it = g_ctx_by_path.find(path);
  if (it != g_ctx_by_path.end() && it->second != nullptr) {
    RtosContext *ctx = it->second;
    if (!ctx->parked) {
      ctx->pause_requested = true;
      if (ctx->queue != nullptr) {
        RtosMsg msg{};
        xQueueSend(ctx->queue, &msg, 0);
      }
    }
  }
  xSemaphoreGive(g_ctx_lock);
}

static void resume_app_script(const std::string &path) {
  ensure_lifecycle_locks();
  if (g_ctx_lock == nullptr) return;
  xSemaphoreTake(g_ctx_lock, portMAX_DELAY);
  auto it = g_ctx_by_path.find(path);
  if (it != g_ctx_by_path.end() && it->second != nullptr) {
    RtosContext *ctx = it->second;
    ctx->pause_requested = false;
    if (ctx->parked && ctx->task != nullptr) {
      vTaskResume(ctx->task);
    }
  }
  xSemaphoreGive(g_ctx_lock);
}

// Set the per-state abort flag and wake the script: a dummy rtos message wakes a task
// blocked in sys.wait, and a parked task is resumed so it can observe the abort flag.
static void stop_app_script(const std::string &path) {
  ensure_lifecycle_locks();
  if (g_ctx_lock == nullptr) return;
  xSemaphoreTake(g_ctx_lock, portMAX_DELAY);
  auto it = g_ctx_by_path.find(path);
  if (it != g_ctx_by_path.end() && it->second != nullptr) {
    RtosContext *ctx = it->second;
    ctx->abort_requested = true;
    ctx->pause_requested = false;
    if (ctx->parked && ctx->task != nullptr) {
      vTaskResume(ctx->task);
    } else if (ctx->queue != nullptr) {
      RtosMsg msg{};
      xQueueSend(ctx->queue, &msg, 0);
    }
  }
  xSemaphoreGive(g_ctx_lock);
}

// Async HTTP helpers used by lua_httplib.cpp.
std::string lua_runtime_script_path(lua_State *L) {
  RtosContext *ctx = get_ctx(L);
  return ctx != nullptr ? ctx->path : std::string();
}

void lua_http_notify(const std::string &owner) {
  ensure_lifecycle_locks();
  if (g_ctx_lock == nullptr) return;
  xSemaphoreTake(g_ctx_lock, portMAX_DELAY);
  auto it = g_ctx_by_path.find(owner);
  if (it != g_ctx_by_path.end() && it->second != nullptr && it->second->queue != nullptr) {
    RtosMsg msg{MSG_HTTP, 0, 0};
    xQueueSend(it->second->queue, &msg, 0);
  }
  xSemaphoreGive(g_ctx_lock);
}

// A run path can still be claimed while its task is winding down after an abort (low
// memory, app-center refresh's abort_all, OTA...). In that window a new launch must wait
// for the old task instead of being silently skipped.
static bool is_script_aborting(const std::string &path) {
  ensure_lifecycle_locks();
  if (g_ctx_lock == nullptr) return false;
  xSemaphoreTake(g_ctx_lock, portMAX_DELAY);
  bool aborting = false;
  auto it = g_ctx_by_path.find(path);
  if (it != g_ctx_by_path.end() && it->second != nullptr) {
    aborting = it->second->abort_requested || it->second->abort_generation != g_abort_generation;
  }
  xSemaphoreGive(g_ctx_lock);
  return aborting;
}

static size_t g_lua_max_used = 0;

// Samples this state's memory from the task that owns it: lua_gc() must never be called from
// another task, so the Lua task records its own numbers at the rtos.receive boundary and the
// settings page just reads the cached values.
static void sample_ctx_memory(lua_State *L, RtosContext *ctx) {
  if (L == nullptr || ctx == nullptr) return;
  int kb = lua_gc(L, LUA_GCCOUNT, 0);
  int bytes = lua_gc(L, LUA_GCCOUNTB, 0);
  ctx->lua_bytes = (uint32_t) kb * 1024u + (uint32_t) bytes;
  UBaseType_t free_words = uxTaskGetStackHighWaterMark(nullptr);
  uint32_t free_bytes = (uint32_t) free_words * sizeof(StackType_t);
  ctx->stack_bytes = free_bytes < LUA_TASK_STACK ? LUA_TASK_STACK - free_bytes : 0;
}

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
  while (ms > 0) {
    lua_abort_if_ota(L);
    int slice = ms > 50 ? 50 : ms;
    vTaskDelay(pdMS_TO_TICKS(slice));
    ms -= slice;
  }
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
  RtosContext *ctx = timer ? timer->ctx : nullptr;
  if (timer == nullptr || ctx == nullptr || !ctx->alive || ctx->queue == nullptr) return;

  RtosMsg msg{MSG_TIMER, timer->id, timer->repeat};
  xQueueSend(ctx->queue, &msg, 0);

  if (timer->repeat == 0) {
    if (ctx->lock) xSemaphoreTake(ctx->lock, portMAX_DELAY);
    ctx->timers.erase(timer->id);
    if (ctx->lock) xSemaphoreGive(ctx->lock);
    timer_cleanup(timer);
  } else if (timer->repeat > 0) {
    timer->repeat--;
    if (timer->repeat == 0) {
      if (ctx->lock) xSemaphoreTake(ctx->lock, portMAX_DELAY);
      ctx->timers.erase(timer->id);
      if (ctx->lock) xSemaphoreGive(ctx->lock);
      timer_cleanup(timer);
    }
  }
}

// Freeze this state's timers before the Lua task parks itself. One-shot timers restart with
// their original timeout after resume (suspended time does not count), periodic timers
// restart with their period.
static void pause_ctx_timers(RtosContext *ctx) {
  if (ctx == nullptr || ctx->lock == nullptr) return;
  xSemaphoreTake(ctx->lock, portMAX_DELAY);
  for (auto &entry : ctx->timers) {
    RtosTimer *timer = entry.second;
    if (timer == nullptr || timer->handle == nullptr) continue;
    timer->paused = esp_timer_stop(timer->handle) == ESP_OK;
  }
  xSemaphoreGive(ctx->lock);
}

static void resume_ctx_timers(RtosContext *ctx) {
  if (ctx == nullptr || ctx->lock == nullptr) return;
  xSemaphoreTake(ctx->lock, portMAX_DELAY);
  for (auto &entry : ctx->timers) {
    RtosTimer *timer = entry.second;
    if (timer == nullptr || timer->handle == nullptr || !timer->paused) continue;
    timer->paused = false;
    if (timer->repeat == 0) {
      esp_timer_start_once(timer->handle, (uint64_t) timer->timeout_ms * 1000);
    } else {
      esp_timer_start_periodic(timer->handle, (uint64_t) timer->timeout_ms * 1000);
    }
  }
  xSemaphoreGive(ctx->lock);
}

// Cooperative suspend: the Lua task parks itself at the rtos.receive boundary, where it
// holds no locks and its timers can be frozen safely. The main task resumes it with
// vTaskResume when the app becomes visible again.
static void maybe_pause_app(lua_State *L, RtosContext *ctx) {
  if (ctx == nullptr || !ctx->pause_requested) return;
  ctx->pause_requested = false;
  pause_ctx_timers(ctx);
  ctx->parked = true;
  vTaskSuspend(nullptr);
  ctx->parked = false;
  if (ctx->abort_requested) {
    luaL_error(L, "Lua script aborted because runtime abort was requested");
  }
  resume_ctx_timers(ctx);
}

static int rtos_receive(lua_State *L) {
  RtosContext *ctx = get_ctx(L);
  if (ctx == nullptr || !ctx->alive || ctx->queue == nullptr) {
    lua_pushinteger(L, -1);
    return 1;
  }

  // Per-app stop is deferred to this boundary: never raise from inside a blocking C call
  // (e.g. esp_http_client), otherwise its close/cleanup is skipped and resources leak.
  if (ctx->abort_requested) {
    luaL_error(L, "Lua script aborted because runtime abort was requested");
  }

  maybe_pause_app(L, ctx);

  // Deliver any async HTTP completions that are already done (safe without the wake
  // message: the worker also sends one, but it can be dropped if the queue is full).
  lua_http_dispatch(L, ctx->path);

  int timeout = (int) luaL_optinteger(L, 1, -1);
  RtosMsg msg{0, 0, 0};
  if (timeout < 0) {
    while (true) {
      lua_abort_if_ota(L);
      // Event-driven USB delivery: runs on this task, which owns the Lua state.
      // If a batch was handed to a Lua callback (which typically sys.publish()es),
      // bail out now so sys.run() can dispatch the message queue; otherwise a
      // waiting coroutine would only wake up on its own timeout timer.
      if (lua_usb_rtos_pump(L)) {
        lua_pushinteger(L, -1);
        return 1;
      }
      if (xQueueReceive(ctx->queue, &msg, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (msg.msgid == MSG_HTTP) {
          lua_http_dispatch(L, ctx->path);
          continue;
        }
        break;
      }
    }
  } else {
    int remaining = timeout;
    while (true) {
      lua_abort_if_ota(L);
      if (lua_usb_rtos_pump(L)) {
        lua_pushinteger(L, -1);
        return 1;
      }
      int slice = remaining > 100 ? 100 : remaining;
      if (xQueueReceive(ctx->queue, &msg, pdMS_TO_TICKS(slice)) == pdTRUE) {
        if (msg.msgid == MSG_HTTP) {
          lua_http_dispatch(L, ctx->path);
          continue;
        }
        break;
      }
      remaining -= slice;
      if (remaining <= 0) {
        lua_pushinteger(L, -1);
        return 1;
      }
    }
  }

  // auto GC policy: run on receive boundary
  if (ctx->autogc_period > 0) {
    ctx->autogc_counter++;
    if (ctx->autogc_counter >= ctx->autogc_period) {
      ctx->autogc_counter = 0;
      size_t total = (size_t) lua_gc(L, LUA_GCCOUNT, 0) * 1024;
      size_t used = total;
      if (total > 0 && (used * 100) >= (total * ctx->autogc_high)) {
        lua_gc(L, LUA_GCCOLLECT, 0);
        lua_gc(L, LUA_GCCOLLECT, 0);
      } else if (total > 0 && (used * 100) >= (total * ctx->autogc_mid)) {
        lua_gc(L, LUA_GCCOLLECT, 0);
        lua_gc(L, LUA_GCCOLLECT, 0);
      }
    }
  }

  // Memory sampling for the settings "running apps" page: cheap, and only from this task.
  if (++ctx->sample_counter >= 20) {
    ctx->sample_counter = 0;
    sample_ctx_memory(L, ctx);
  }

  lua_pushinteger(L, msg.msgid);
  lua_pushinteger(L, msg.id);
  lua_pushinteger(L, msg.repeat);
  return 3;
}

static int rtos_timer_start(lua_State *L) {
  RtosContext *ctx = get_ctx(L);
  if (ctx == nullptr || !ctx->alive) {
    ESP_LOGE(TAG, "rtos.timer_start: no context");
    lua_pushinteger(L, 0);
    return 1;
  }
  if (ctx->queue == nullptr) {
    ESP_LOGE(TAG, "rtos.timer_start: queue null");
  }
  if (ctx->lock == nullptr) {
    ESP_LOGE(TAG, "rtos.timer_start: lock null");
  }
  int id = (int) luaL_checkinteger(L, 1);
  int timeout = (int) luaL_checkinteger(L, 2);
  int repeat = (int) luaL_optinteger(L, 3, 0);
  if (timeout < 1) {
    lua_pushinteger(L, 0);
    return 1;
  }

  if (ctx->lock) xSemaphoreTake(ctx->lock, portMAX_DELAY);
  auto it = ctx->timers.find(id);
  if (it != ctx->timers.end()) {
    timer_cleanup(it->second);
    ctx->timers.erase(it);
  }
  if (ctx->lock) xSemaphoreGive(ctx->lock);

  auto *timer = new RtosTimer{ctx, id, repeat, (uint32_t) timeout, nullptr};

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

  if (ctx->lock) xSemaphoreTake(ctx->lock, portMAX_DELAY);
  ctx->timers[id] = timer;
  if (ctx->lock) xSemaphoreGive(ctx->lock);

  if (repeat == 0) {
    err = esp_timer_start_once(timer->handle, (uint64_t) timeout * 1000);
  } else {
    err = esp_timer_start_periodic(timer->handle, (uint64_t) timeout * 1000);
  }

  if (err != ESP_OK) {
    ESP_LOGW(TAG, "rtos.timer_start start fail: %s", esp_err_to_name(err));
    if (ctx->lock) xSemaphoreTake(ctx->lock, portMAX_DELAY);
    ctx->timers.erase(id);
    if (ctx->lock) xSemaphoreGive(ctx->lock);
    timer_cleanup(timer);
    lua_pushinteger(L, 0);
    return 1;
  }

  lua_pushinteger(L, 1);
  return 1;
}

static int rtos_timer_stop(lua_State *L) {
  RtosContext *ctx = get_ctx(L);
  if (ctx == nullptr || !ctx->alive) return 0;
  if (!lua_isinteger(L, 1)) return 0;
  int id = (int) lua_tointeger(L, 1);

  if (ctx->lock) xSemaphoreTake(ctx->lock, portMAX_DELAY);
  auto it = ctx->timers.find(id);
  if (it != ctx->timers.end()) {
    auto *timer = it->second;
    ctx->timers.erase(it);
    if (ctx->lock) xSemaphoreGive(ctx->lock);
    timer_cleanup(timer);
    return 0;
  }
  if (ctx->lock) xSemaphoreGive(ctx->lock);
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
  RtosContext *ctx = get_ctx(L);
  if (ctx == nullptr || !ctx->alive) return 0;
  uint32_t period = (uint32_t) luaL_optinteger(L, 1, 100);
  uint32_t mid = (uint32_t) luaL_optinteger(L, 2, 80);
  uint32_t high = (uint32_t) luaL_optinteger(L, 3, 90);
  if (period > 60000) return 0;
  if (mid > 95 || high > 95) return 0;
  if (mid < 50 || high < 50) return 0;
  if (mid >= high) return 0;
  ctx->autogc_period = period;
  ctx->autogc_mid = mid;
  ctx->autogc_high = high;
  ctx->autogc_counter = 0;
  return 0;
}
static uint32_t random_bounded_u32(uint32_t bound) {
  if (bound == 0) return 0;
  const uint32_t threshold = static_cast<uint32_t>(-bound) % bound;
  while (true) {
    uint32_t r = esp_random();
    if (r >= threshold) return r % bound;
  }
}

static int lua_esp_random(lua_State *L) {
  int argc = lua_gettop(L);
  if (argc == 0) {
    // Keep default return in Lua integer range.
    uint32_t bound = static_cast<uint32_t>(LUA_MAXINTEGER) + 1U;
    lua_pushinteger(L, static_cast<lua_Integer>(random_bounded_u32(bound)));
    return 1;
  }

  if (argc == 1) {
    lua_Integer upper = luaL_checkinteger(L, 1);
    luaL_argcheck(L, upper >= 1, 1, "upper bound must be >= 1");
    uint64_t span = static_cast<uint64_t>(upper);
    luaL_argcheck(L, span <= 0xFFFFFFFFULL, 1, "range too large");
    lua_pushinteger(L, static_cast<lua_Integer>(random_bounded_u32(static_cast<uint32_t>(span)) + 1U));
    return 1;
  }

  lua_Integer lower = luaL_checkinteger(L, 1);
  lua_Integer upper = luaL_checkinteger(L, 2);
  luaL_argcheck(L, lower <= upper, 2, "lower bound must be <= upper bound");

  uint64_t span = static_cast<uint64_t>(upper - lower) + 1ULL;
  luaL_argcheck(L, span <= 0xFFFFFFFFULL, 2, "range too large");
  lua_Integer out = lower + static_cast<lua_Integer>(random_bounded_u32(static_cast<uint32_t>(span)));
  lua_pushinteger(L, out);
  return 1;
}

static int lua_esp_mac(lua_State *L) {
  uint8_t mac[6] = {0};
  if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) {
    lua_pushnil(L);
    return 1;
  }
  char mac_str[18];
  std::snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4],
                mac[5]);
  lua_pushstring(L, mac_str);
  return 1;
}

static std::string load_uid_from_preferences() {
  if (global_preferences == nullptr) return "0000";

  std::array<char, UID_RESTORE_SIZE> temp{};
  auto pref = global_preferences->make_preference<std::array<char, UID_RESTORE_SIZE>>(
      GLOBAL_PREFERENCE_SEED ^ UID_NAME_HASH);
  if (!pref.load(&temp)) return "0000";

  size_t len = static_cast<unsigned char>(temp[0]);
  if (len >= UID_RESTORE_SIZE) len = UID_RESTORE_SIZE - 1;
  return std::string(temp.data() + 1, len);
}

static int lua_lemonade_uid(lua_State *L) {
  auto *runtime = static_cast<LuaRuntime *>(lua_touserdata(L, lua_upvalueindex(1)));
  std::string uid = runtime != nullptr ? runtime->get_uid() : "0000";
  if (uid.empty()) {
    lua_pushnil(L);
    return 1;
  }
  lua_pushstring(L, uid.c_str());
  return 1;
}

static void rtos_cleanup(RtosContext &ctx) {
  ctx.alive = false;
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
static void register_esp_api(lua_State *L) {
  lua_newtable(L);  // esp

  lua_pushcfunction(L, lua_esp_random);
  lua_setfield(L, -2, "random");
  lua_pushcfunction(L, lua_esp_mac);
  lua_setfield(L, -2, "mac");

  lua_setglobal(L, "esp");
}

static void register_lemonade_api(lua_State *L, LuaRuntime *runtime) {
  lua_newtable(L);  // lemonade

  lua_pushlightuserdata(L, runtime);
  lua_pushcclosure(L, lua_lemonade_uid, 1);
  lua_setfield(L, -2, "uid");

  lua_setglobal(L, "lemonade");
}

static void register_base_api(lua_State *L, const std::string &script_path, LuaRuntime *runtime) {
  register_log_api(L);
  register_rtos_api(L);
  register_esp_api(L);
  register_lemonade_api(L, runtime);
  register_app_api(L, script_path);
  register_esphome_api(L);
  register_usb_api(L);
  register_fs_api(L);
  register_lvgl_api(L, script_path);
  register_fskv_api(L, script_path);
  luaL_requiref(L, "json", luaopen_json, 1);
  lua_pop(L, 1);
  luaL_requiref(L, "http", luaopen_http, 1);
  lua_pop(L, 1);

  lua_pushcfunction(L, lua_delay_ms);
  lua_setglobal(L, "delay_ms");
}
#endif

void LuaRuntime::setup() {
#ifndef LUA_RUNTIME_STUB
  set_lvgl_owner_task((void *) xTaskGetCurrentTaskHandle());
  this->cached_uid_ = load_uid_from_preferences();
  lua_usb_attach(this->usb_source_);
#endif
}

void LuaRuntime::loop() {
#ifndef LUA_RUNTIME_STUB
  process_lvgl_jobs();
  tick_app_page_launch();
  update_app_lifecycle();
  lua_usb_pump_tx();
#endif
}

void LuaRuntime::dump_config() {
  ESP_LOGCONFIG(TAG, "Lua Runtime");
#ifdef LUA_RUNTIME_STUB
  ESP_LOGCONFIG(TAG, "  Mode: stub (no Lua linked)");
#else
  ESP_LOGCONFIG(TAG, "  Mode: enabled");
  ESP_LOGCONFIG(TAG, "  Async core: %d", this->async_core_);
#endif
}

void LuaRuntime::mark_task_done(const std::string &path) {
#ifndef LUA_RUNTIME_STUB
  if (is_main_script(path)) {
    // The script is gone: drop its widgets and let the next launch rebuild them.
    release_app_page(app_dir_of_script(path));
  }
#endif
  release_run_path(path);
#ifndef LUA_RUNTIME_STUB
  // A launch arrived while this task was aborting: start it now that the path is free.
  if (take_pending_restart(path)) {
    ESP_LOGI(TAG, "Restarting Lua script after previous task exited: %s", path.c_str());
    if (is_main_script(path)) {
      // The 3 s timeout may already have dismissed the cover; re-arm the request so the
      // rebuilt page is shown when it reports ready.
      rearm_app_page_launch(app_dir_of_script(path));
    }
    this->run_file_async(path);
  }
#endif
}

// Parks background apps (task suspended, timers frozen) and only stops one when free PSRAM
// is critically low. Locked/asleep disables stopping, not parking.
void LuaRuntime::update_app_lifecycle() {
#ifndef LUA_RUNTIME_STUB
  const bool gate_open = !this->lifecycle_gate_ || this->lifecycle_gate_();

  std::vector<std::string> tasks;
  if (g_ctx_lock != nullptr) {
    xSemaphoreTake(g_ctx_lock, portMAX_DELAY);
    tasks.reserve(g_ctx_by_path.size());
    for (const auto &entry : g_ctx_by_path) {
      RtosContext *ctx = entry.second;
      if (ctx == nullptr || !is_main_script(entry.first)) continue;
      // Never park a synchronous run_file() that is executing on this task.
      if (ctx->task == xTaskGetCurrentTaskHandle()) continue;
      tasks.push_back(entry.first);
    }
    xSemaphoreGive(g_ctx_lock);
  }
  if (tasks.empty()) {
    g_app_background_since.clear();
    g_app_pause_sent.clear();
    return;
  }

  // While locked or asleep every app counts as background, so the foreground app is parked
  // too (timers frozen). It is resumed when the gate opens and its page is active again.
  const std::string active = gate_open ? active_app_dir() : std::string();
  const uint32_t now = (uint32_t) (xTaskGetTickCount() * portTICK_PERIOD_MS);

  auto is_running_dir = [&tasks](const std::string &dir) {
    for (const auto &path : tasks) {
      if (app_dir_of_script(path) == dir) return true;
    }
    return false;
  };

  // Forget apps that are no longer running.
  for (auto it = g_app_background_since.begin(); it != g_app_background_since.end();) {
    if (!is_running_dir(it->first)) {
      it = g_app_background_since.erase(it);
    } else {
      ++it;
    }
  }
  for (auto it = g_app_pause_sent.begin(); it != g_app_pause_sent.end();) {
    if (g_app_background_since.find(*it) == g_app_background_since.end()) {
      it = g_app_pause_sent.erase(it);
    } else {
      ++it;
    }
  }

  for (const auto &path : tasks) {
    const std::string dir = app_dir_of_script(path);

    if (!active.empty() && dir == active) {
      g_app_background_since.erase(dir);
      g_app_pause_sent.erase(dir);
      resume_app_script(path);
      continue;
    }

    auto it = g_app_background_since.find(dir);
    if (it == g_app_background_since.end()) {
      g_app_background_since[dir] = now;
      continue;
    }
    const uint32_t bg_ms = (uint32_t) (now - it->second);

    // Park the app once its UI exists; a script that is still building is left alone.
    if (bg_ms >= APP_BACKGROUND_PAUSE_MS && !is_app_launching(dir) && is_app_page_ready(dir) &&
        g_app_pause_sent.find(dir) == g_app_pause_sent.end()) {
      pause_app_script(path);
      g_app_pause_sent.insert(dir);
    }
  }

  // Emergency only: reclaim memory by stopping the oldest background app. Never stop the
  // foreground page or an app that is still launching (its page is not active yet), and
  // leave a cooldown between two stops so the aborted task has time to release its PSRAM.
  if (!gate_open) return;
  if (heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) >= APP_LOW_PSRAM_BYTES) return;
  static uint32_t last_low_mem_stop = 0;
  if (last_low_mem_stop != 0 && (uint32_t) (now - last_low_mem_stop) < 1500) return;

  std::string oldest;
  uint32_t oldest_at = 0;
  for (const auto &entry : g_app_background_since) {
    if (!active.empty() && entry.first == active) continue;
    if (is_app_launching(entry.first)) continue;
    if (oldest.empty() || (int32_t) (entry.second - oldest_at) < 0) {
      oldest = entry.first;
      oldest_at = entry.second;
    }
  }
  if (oldest.empty()) return;
  for (const auto &path : tasks) {
    if (app_dir_of_script(path) == oldest) {
      ESP_LOGW(TAG, "PSRAM low, stopping background app: %s", path.c_str());
      stop_app_script(path);
      last_low_mem_stop = now;
      g_app_pause_sent.erase(oldest);
      g_app_background_since.erase(oldest);
      break;
    }
  }
#endif
}

void LuaRuntime::set_ota_active(bool active) {
#ifndef LUA_RUNTIME_STUB
  g_ota_active = active;
  ESP_LOGI(TAG, "OTA guard %s", active ? "enabled" : "disabled");
#else
  (void) active;
#endif
}

void LuaRuntime::register_rtttl_player(const std::string &name, rtttl::Rtttl *player) {
#ifndef LUA_RUNTIME_STUB
  lua_runtime::register_rtttl_player(name, player);
#else
  (void) name;
  (void) player;
#endif
}

void LuaRuntime::register_media_player(const std::string &name, media_player::MediaPlayer *player) {
#ifndef LUA_RUNTIME_STUB
  lua_runtime::register_media_player(name, player);
#else
  (void) name;
  (void) player;
#endif
}

bool LuaRuntime::is_ota_active() const {
#ifndef LUA_RUNTIME_STUB
  return g_ota_active;
#else
  return false;
#endif
}

void LuaRuntime::abort_all() {
#ifndef LUA_RUNTIME_STUB
  g_abort_generation++;
  ESP_LOGI(TAG, "Requested abort for all running Lua scripts");

  // Bumping the generation alone is not enough: a parked (suspended) task never runs, so it
  // would never observe the new generation, keep its run path claimed and stay listed as
  // "stopping" forever. Wake every script explicitly so each one exits at its next
  // boundary.
  std::vector<std::string> paths;
  ensure_lifecycle_locks();
  if (g_ctx_lock != nullptr) {
    xSemaphoreTake(g_ctx_lock, portMAX_DELAY);
    paths.reserve(g_ctx_by_path.size());
    for (const auto &entry : g_ctx_by_path) paths.push_back(entry.first);
    xSemaphoreGive(g_ctx_lock);
  }
  for (const auto &path : paths) stop_app_script(path);
#endif
}

// Reads the display name out of <dir>/manifest.json without pulling in a JSON dependency:
// the manifest is a flat object, so the first "name" key is the app name.
static std::string app_display_name(const std::string &dir) {
  static std::unordered_map<std::string, std::string> cache;
  auto cached = cache.find(dir);
  if (cached != cache.end()) return cached->second;

  std::string name;
  FILE *file = fopen((dir + "/manifest.json").c_str(), "r");
  if (file != nullptr) {
    char buffer[1024];
    size_t len = fread(buffer, 1, sizeof(buffer) - 1, file);
    buffer[len] = '\0';
    fclose(file);

    std::string body(buffer, len);
    size_t key = body.find("\"name\"");
    if (key != std::string::npos) {
      size_t colon = body.find(':', key + 6);
      size_t quote = colon == std::string::npos ? std::string::npos : body.find('"', colon);
      if (quote != std::string::npos) {
        size_t end = quote + 1;
        while (end < body.size() && body[end] != '"') {
          if (body[end] == '\\') end++;
          end++;
        }
        if (end <= body.size()) name = body.substr(quote + 1, end - quote - 1);
      }
    }
  }
  if (name.empty()) {
    size_t slash = dir.find_last_of('/');
    name = slash == std::string::npos ? dir : dir.substr(slash + 1);
  }
  cache[dir] = name;
  return name;
}

std::vector<LuaAppInfo> LuaRuntime::list_apps() {
  std::vector<LuaAppInfo> apps;
#ifndef LUA_RUNTIME_STUB
  ensure_lifecycle_locks();
  if (g_ctx_lock == nullptr) return apps;

  const uint32_t now = (uint32_t) (xTaskGetTickCount() * portTICK_PERIOD_MS);
  xSemaphoreTake(g_ctx_lock, portMAX_DELAY);
  for (const auto &entry : g_ctx_by_path) {
    RtosContext *ctx = entry.second;
    if (ctx == nullptr || !is_main_script(entry.first)) continue;
    LuaAppInfo info;
    info.dir = app_dir_of_script(entry.first);
    info.name = app_display_name(info.dir);
    info.script = entry.first;
    info.lua_bytes = ctx->lua_bytes;
    info.stack_bytes = ctx->stack_bytes;
    info.parked = ctx->parked;
    // Only an explicit stop (per-app stop or abort_all, which now sets the flag) counts as
    // "stopping". A stale generation mismatch must not keep every app stuck on it.
    info.aborting = ctx->abort_requested;
    if (ctx->started_at != 0) info.running_ms = (uint32_t) (now - ctx->started_at);
    apps.push_back(std::move(info));
  }
  xSemaphoreGive(g_ctx_lock);

  // Oldest first: the list is stable between refreshes.
  std::sort(apps.begin(), apps.end(), [](const LuaAppInfo &a, const LuaAppInfo &b) {
    return a.running_ms > b.running_ms;
  });
#endif
  return apps;
}

bool LuaRuntime::stop_app(const std::string &app_dir) {
#ifdef LUA_RUNTIME_STUB
  (void) app_dir;
  return false;
#else
  if (app_dir.empty()) return false;
  const std::string path = app_dir + "/main.lua";
  RtosContext *ctx = nullptr;
  ensure_lifecycle_locks();
  if (g_ctx_lock != nullptr) {
    xSemaphoreTake(g_ctx_lock, portMAX_DELAY);
    auto it = g_ctx_by_path.find(path);
    if (it != g_ctx_by_path.end()) ctx = it->second;
    xSemaphoreGive(g_ctx_lock);
  }
  if (ctx == nullptr) return false;
  ESP_LOGI(TAG, "Stopping app from settings: %s", app_dir.c_str());
  // Aborted here, released by the task itself (page widgets go away in mark_task_done).
  stop_app_script(path);
  return true;
#endif
}

LuaMemoryInfo LuaRuntime::memory_info() {
  LuaMemoryInfo info;
  multi_heap_info_t heap{};
  heap_caps_get_info(&heap, MALLOC_CAP_SPIRAM);
  info.psram_free = heap.total_free_bytes;
  info.psram_largest = heap.largest_free_block;
  info.psram_total = heap.total_free_bytes + heap.total_allocated_bytes;
  multi_heap_info_t internal{};
  heap_caps_get_info(&internal, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  info.internal_free = internal.total_free_bytes;
  info.internal_largest = internal.largest_free_block;
  info.internal_total = internal.total_free_bytes + internal.total_allocated_bytes;

#ifndef LUA_RUNTIME_STUB
  ensure_lifecycle_locks();
  if (g_ctx_lock != nullptr) {
    xSemaphoreTake(g_ctx_lock, portMAX_DELAY);
    for (const auto &entry : g_ctx_by_path) {
      RtosContext *ctx = entry.second;
      if (ctx != nullptr && is_main_script(entry.first)) info.lua_bytes_total += ctx->lua_bytes;
    }
    xSemaphoreGive(g_ctx_lock);
  }
#endif
  return info;
}

bool LuaRuntime::run_file(const std::string &path) {
#ifdef LUA_RUNTIME_STUB
  ESP_LOGE(TAG, "Lua runtime is in stub mode. Add Lua sources and disable enable_stub.");
  ESP_LOGE(TAG, "Requested: %s", path.c_str());
  return false;
#else
  if (ota_is_active()) {
    ESP_LOGW(TAG, "Skip Lua file while OTA is in progress: %s", path.c_str());
    return false;
  }

  lua_State *L = luaL_newstate();
  if (L == nullptr) {
    ESP_LOGE(TAG, "Failed to create Lua state");
    return false;
  }

  RtosContext ctx;
  ctx.queue = xQueueCreate(16, sizeof(RtosMsg));
  ctx.lock = xSemaphoreCreateMutex();
  ctx.task = xTaskGetCurrentTaskHandle();
  ctx.path = path;
  ctx.abort_generation = g_abort_generation;
  ctx.started_at = (uint32_t) (xTaskGetTickCount() * portTICK_PERIOD_MS);
  set_ctx(L, &ctx);
  register_ctx(path, &ctx);

  if (ctx.queue == nullptr) {
    ESP_LOGE(TAG, "rtos.queue create failed");
  }
  if (ctx.lock == nullptr) {
    ESP_LOGE(TAG, "rtos.lock create failed");
  }

  // esp_timer is initialized by the ESP-IDF/ESPHome startup and esp_timer_create()
  // initializes it lazily if needed. Calling esp_timer_init() here for every script just
  // logs "Task is already initialized" (once per app script).

  luaL_openlibs(L);
  set_abort_generation(L, g_abort_generation);
  ctx.abort_generation = g_abort_generation;
  register_base_api(L, path, this);
  set_package_path(L, path);
  lua_sethook(L, lua_ota_hook, LUA_MASKCOUNT, 1000);

  int load_status = luaL_loadfilex(L, path.c_str(), nullptr);
  if (load_status != LUA_OK) {
    const char *err = lua_tostring(L, -1);
    std::string error_message = err ? err : "(unknown)";
    ESP_LOGE(TAG, "Lua load error: %s", error_message.c_str());
    cleanup_lvgl_api(L);
    lua_http_cancel_owner(L, path);
    lua_close(L);
    unregister_ctx(path);
    rtos_cleanup(ctx);
    show_lua_error_on_app_page(path, error_message);
    return false;
  }

  int call_status = lua_pcall(L, 0, LUA_MULTRET, 0);
  if (call_status != LUA_OK) {
    const char *err = lua_tostring(L, -1);
    std::string error_message = err ? err : "(unknown)";
    // Lua prefixes the chunk/line (e.g. "lib/sys.lua:578: ..."), so compare substrings.
    bool expected_abort =
        error_message.find("Lua script aborted because runtime abort was requested") != std::string::npos ||
        error_message.find("Lua script aborted because OTA is in progress") != std::string::npos;
    if (expected_abort) {
      ESP_LOGI(TAG, "Lua script stopped: %s", path.c_str());
    } else {
      ESP_LOGE(TAG, "Lua runtime error: %s", error_message.c_str());
    }
    cleanup_lvgl_api(L);
    lua_http_cancel_owner(L, path);
    lua_close(L);
    unregister_ctx(path);
    rtos_cleanup(ctx);
    if (!expected_abort) {
      show_lua_error_on_app_page(path, error_message);
    }
    return false;
  }

  cleanup_lvgl_api(L);
  lua_http_cancel_owner(L, path);
  lua_close(L);
  unregister_ctx(path);
  rtos_cleanup(ctx);
  ESP_LOGI(TAG, "Lua script finished: %s", path.c_str());
  return true;
#endif
}

struct LuaTaskArgs {
  LuaRuntime *self;
  std::string path;
  bool use_caps_delete;
};

static void lua_task_entry(void *param) {
  auto *args = static_cast<LuaTaskArgs *>(param);
  LuaRuntime *self = args->self;
  std::string path = args->path;
  bool use_caps_delete = args->use_caps_delete;
  delete args;

  self->run_file(path);
  self->mark_task_done(path);
  if (use_caps_delete) {
    vTaskDeleteWithCaps(nullptr);
  } else {
    vTaskDelete(nullptr);
  }
}

bool LuaRuntime::run_file_async(const std::string &path) {
#ifdef LUA_RUNTIME_STUB
  ESP_LOGE(TAG, "Lua runtime is in stub mode. Add Lua sources and disable enable_stub.");
  ESP_LOGE(TAG, "Requested: %s", path.c_str());
  return false;
#else
  if (ota_is_active()) {
    ESP_LOGW(TAG, "Skip async Lua file while OTA is in progress: %s", path.c_str());
    return false;
  }
  if (!claim_run_path(path)) {
    // A new launch can arrive while the previous task is still winding down after an abort
    // (low-memory stop, app-center abort_all, OTA). Remember it so mark_task_done can start
    // it as soon as the run path is released, instead of skipping the launch silently.
    if (is_script_aborting(path)) mark_pending_restart(path);
    ESP_LOGI(TAG, "Lua task already running, skip: %s", path.c_str());
    return false;
  }
  auto *args = new LuaTaskArgs{this, path, true};
  size_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  size_t internal_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  size_t psram_largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  ESP_LOGI(TAG,
           "Lua task memory before create: internal_free=%u internal_largest=%u psram_free=%u psram_largest=%u stack=%u",
           static_cast<unsigned>(internal_free), static_cast<unsigned>(internal_largest), static_cast<unsigned>(psram_free),
           static_cast<unsigned>(psram_largest), static_cast<unsigned>(LUA_TASK_STACK));

  BaseType_t ok = xTaskCreatePinnedToCoreWithCaps(lua_task_entry, "lua_task", LUA_TASK_STACK, args, LUA_TASK_PRIO, nullptr,
                                                  this->async_core_, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (ok != pdPASS) {
    ESP_LOGW(TAG, "Lua task create in PSRAM failed, retrying with internal RAM");
    args->use_caps_delete = false;
    ok = xTaskCreatePinnedToCore(lua_task_entry, "lua_task", LUA_TASK_STACK, args, LUA_TASK_PRIO, nullptr,
                                 this->async_core_);
  }
  if (ok != pdPASS) {
    internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    internal_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    psram_largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_LOGE(TAG,
             "Failed to create Lua task on core %d internal_free=%u internal_largest=%u psram_free=%u psram_largest=%u stack=%u",
             this->async_core_, static_cast<unsigned>(internal_free), static_cast<unsigned>(internal_largest),
             static_cast<unsigned>(psram_free), static_cast<unsigned>(psram_largest),
             static_cast<unsigned>(LUA_TASK_STACK));
    delete args;
    release_run_path(path);
    return false;
  }
  return true;
#endif
}

}  // namespace lua_runtime
}  // namespace esphome

