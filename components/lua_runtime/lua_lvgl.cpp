#include "lua_lvgl.h"

#include <cctype>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "esphome/core/log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#ifdef LUA_RUNTIME_STUB
struct lua_State;
struct lv_obj_t;

namespace esphome {
namespace lua_runtime {

void register_app_page(const std::string &, lv_obj_t *) {}
void unregister_app_page(const std::string &) {}
bool is_active_app_page() { return false; }
void register_lvgl_api(lua_State *, const std::string &) {}
void cleanup_lvgl_api(lua_State *) {}
void show_lua_error_on_app_page(const std::string &, const std::string &) {}
void set_lvgl_owner_task(void *) {}
void process_lvgl_jobs() {}

}  // namespace lua_runtime
}  // namespace esphome

#else
#include "lua.hpp"
#include "lvgl.h"

namespace esphome {
namespace lua_runtime {

static const char *TAG = "lua_lvgl";
static SemaphoreHandle_t g_page_lock = nullptr;
static std::unordered_map<std::string, lv_obj_t *> g_app_pages;
static const char *LVGL_APP_PAGE_KEY = "lua_lvgl.app_page";
static const char *LVGL_CTX_KEY = "lua_lvgl.ctx";
static const char *LVGL_SCRIPT_DIR_KEY = "lua_lvgl.script_dir";
static constexpr int32_t NOTIFICATION_BAR_Y_HIDE = -45;
static constexpr int32_t NOTIFICATION_BAR_Y_SHOW = 10;
static constexpr uint32_t LUA_NOTIFICATION_MAGIC = 0x4C55494E;

struct LuaEventCb;
struct LuaTimerCb;

struct LuaNotificationBar {
  uint32_t magic;
  lv_obj_t *bar;
  lv_timer_t *close_timer;
  bool closing;
};

enum class LuaFontKind : uint8_t {
  BIN = 0,
  TINY_TTF = 1,
};

struct LuaLvglContext {
  QueueHandle_t queue{nullptr};
  volatile bool alive{true};
  bool fatal_error_reported{false};
  std::string script_path;
  std::unordered_set<LuaEventCb *> event_cbs;
  std::unordered_map<lv_timer_t *, LuaTimerCb *> timers;
  std::unordered_set<LuaTimerCb *> retired_timers;
  std::unordered_map<lv_font_t *, LuaFontKind> fonts;
};
static void ensure_page_lock() {
  if (g_page_lock == nullptr) {
    g_page_lock = xSemaphoreCreateMutex();
  }
}

void register_app_page(const std::string &app_dir, lv_obj_t *page) {
  ensure_page_lock();
  if (g_page_lock) xSemaphoreTake(g_page_lock, portMAX_DELAY);
  g_app_pages[app_dir] = page;
  if (g_page_lock) xSemaphoreGive(g_page_lock);
  ESP_LOGD(TAG, "register app page: %s -> %p", app_dir.c_str(), page);
}

void unregister_app_page(const std::string &app_dir) {
  ensure_page_lock();
  if (g_page_lock) xSemaphoreTake(g_page_lock, portMAX_DELAY);
  g_app_pages.erase(app_dir);
  if (g_page_lock) xSemaphoreGive(g_page_lock);
  ESP_LOGD(TAG, "unregister app page: %s", app_dir.c_str());
}

static lv_obj_t *get_app_page(const std::string &app_dir) {
  ensure_page_lock();
  if (g_page_lock) xSemaphoreTake(g_page_lock, portMAX_DELAY);
  lv_obj_t *page = nullptr;
  auto it = g_app_pages.find(app_dir);
  if (it != g_app_pages.end()) page = it->second;
  if (g_page_lock) xSemaphoreGive(g_page_lock);
  return page;
}

static bool is_registered_app_page(lv_obj_t *page) {
  if (page == nullptr)
    return false;
  ensure_page_lock();
  if (g_page_lock) xSemaphoreTake(g_page_lock, portMAX_DELAY);
  bool found = false;
  for (const auto &entry : g_app_pages) {
    if (entry.second == page) {
      found = true;
      break;
    }
  }
  if (g_page_lock) xSemaphoreGive(g_page_lock);
  return found;
}

bool is_active_app_page() {
  return is_registered_app_page(lv_scr_act());
}

static std::string dir_from_path(const std::string &path) {
  size_t pos = path.find_last_of('/');
  if (pos == std::string::npos) return ".";
  if (pos == 0) return "/";
  return path.substr(0, pos);
}

static std::string shorten_error_message(const std::string &message, size_t limit) {
  if (message.size() <= limit) return message;
  if (limit <= 3) return message.substr(0, limit);
  return message.substr(0, limit - 3) + "...";
}

static void set_app_page(lua_State *L, lv_obj_t *page) {
  lua_pushlightuserdata(L, (void *) &LVGL_APP_PAGE_KEY);
  lua_pushlightuserdata(L, page);
  lua_settable(L, LUA_REGISTRYINDEX);
}

static lv_obj_t *get_app_page_from_lua(lua_State *L) {
  lua_pushlightuserdata(L, (void *) &LVGL_APP_PAGE_KEY);
  lua_gettable(L, LUA_REGISTRYINDEX);
  auto *page = static_cast<lv_obj_t *>(lua_touserdata(L, -1));
  lua_pop(L, 1);
  return page;
}

static void set_script_dir(lua_State *L, const std::string &dir) {
  lua_pushlightuserdata(L, (void *) &LVGL_SCRIPT_DIR_KEY);
  lua_pushstring(L, dir.c_str());
  lua_settable(L, LUA_REGISTRYINDEX);
}

static std::string get_script_dir(lua_State *L) {
  lua_pushlightuserdata(L, (void *) &LVGL_SCRIPT_DIR_KEY);
  lua_gettable(L, LUA_REGISTRYINDEX);
  const char *dir = lua_tostring(L, -1);
  std::string out = dir ? dir : ".";
  lua_pop(L, 1);
  return out;
}

static void set_lvgl_ctx(lua_State *L, LuaLvglContext *ctx) {
  lua_pushlightuserdata(L, (void *) &LVGL_CTX_KEY);
  lua_pushlightuserdata(L, ctx);
  lua_settable(L, LUA_REGISTRYINDEX);
}

static LuaLvglContext *get_lvgl_ctx(lua_State *L) {
  lua_pushlightuserdata(L, (void *) &LVGL_CTX_KEY);
  lua_gettable(L, LUA_REGISTRYINDEX);
  auto *ctx = static_cast<LuaLvglContext *>(lua_touserdata(L, -1));
  lua_pop(L, 1);
  return ctx;
}

static int l_app_page(lua_State *L) {
  lv_obj_t *page = get_app_page_from_lua(L);
  lua_pushlightuserdata(L, page);
  return 1;
}

static lv_obj_t *check_obj(lua_State *L, int idx) {
  if (!lua_islightuserdata(L, idx)) return nullptr;
  return static_cast<lv_obj_t *>(lua_touserdata(L, idx));
}

static QueueHandle_t g_lvgl_job_queue = nullptr;
static void *g_lvgl_owner_task = nullptr;
static SemaphoreHandle_t g_lvgl_batch_lock = nullptr;
static std::unordered_map<void *, int> g_lvgl_batch_depths;

struct LvglJobBase {
  TaskHandle_t waiter{nullptr};
  volatile bool done{false};
  virtual ~LvglJobBase() = default;
  virtual void run() = 0;
};

template<typename F> struct LvglJobVoid : public LvglJobBase {
  explicit LvglJobVoid(F &&fn) : fn_(std::forward<F>(fn)) {}
  void run() override { this->fn_(); }
  F fn_;
};

template<typename R, typename F> struct LvglJobRet : public LvglJobBase {
  explicit LvglJobRet(F &&fn) : fn_(std::forward<F>(fn)) {}
  void run() override { this->result = this->fn_(); }
  F fn_;
  R result{};
};

static void ensure_lvgl_job_queue() {
  if (g_lvgl_job_queue == nullptr) {
    g_lvgl_job_queue = xQueueCreate(32, sizeof(LvglJobBase *));
  }
  if (g_lvgl_batch_lock == nullptr) {
    g_lvgl_batch_lock = xSemaphoreCreateMutex();
  }
}

void set_lvgl_owner_task(void *task_handle) {
  g_lvgl_owner_task = task_handle;
  ensure_lvgl_job_queue();
}

void process_lvgl_jobs() {
  ensure_lvgl_job_queue();
  if (g_lvgl_job_queue == nullptr) return;
  LvglJobBase *job = nullptr;
  int processed = 0;
  while (xQueueReceive(g_lvgl_job_queue, &job, 0) == pdTRUE) {
    if (job != nullptr) {
      job->run();
      job->done = true;
      if (job->waiter != nullptr) xTaskNotifyGive(job->waiter);
      if (job->waiter == nullptr) delete job;
    }
    if (++processed >= 256) break;
  }
}

static void wait_job_done(LvglJobBase *job) {
  while (job != nullptr && !job->done) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
  }
}

static int get_lvgl_batch_depth_for_task(void *task) {
  ensure_lvgl_job_queue();
  if (g_lvgl_batch_lock == nullptr || task == nullptr) return 0;
  xSemaphoreTake(g_lvgl_batch_lock, portMAX_DELAY);
  int depth = 0;
  auto it = g_lvgl_batch_depths.find(task);
  if (it != g_lvgl_batch_depths.end()) depth = it->second;
  xSemaphoreGive(g_lvgl_batch_lock);
  return depth;
}

static void set_lvgl_batch_depth_for_task(void *task, int depth) {
  ensure_lvgl_job_queue();
  if (g_lvgl_batch_lock == nullptr || task == nullptr) return;
  xSemaphoreTake(g_lvgl_batch_lock, portMAX_DELAY);
  if (depth > 0) {
    g_lvgl_batch_depths[task] = depth;
  } else {
    g_lvgl_batch_depths.erase(task);
  }
  xSemaphoreGive(g_lvgl_batch_lock);
}

static void flush_lvgl_jobs_for_current_task() {
  ensure_lvgl_job_queue();
  void *self = (void *) xTaskGetCurrentTaskHandle();
  if (g_lvgl_owner_task == nullptr || self == g_lvgl_owner_task || g_lvgl_job_queue == nullptr) return;

  LvglJobVoid<std::function<void()>> job([]() {});
  job.waiter = xTaskGetCurrentTaskHandle();
  LvglJobBase *base = &job;
  if (xQueueSend(g_lvgl_job_queue, &base, portMAX_DELAY) != pdTRUE) {
    ESP_LOGW(TAG, "flush_lvgl_jobs_for_current_task: queue send failed");
    job.run();
    return;
  }
  wait_job_done(&job);
}

template<typename F> static void lvgl_call_void(F &&fn) {
  ensure_lvgl_job_queue();
  void *self = (void *) xTaskGetCurrentTaskHandle();
  if (g_lvgl_owner_task == nullptr || self == g_lvgl_owner_task || g_lvgl_job_queue == nullptr) {
    fn();
    return;
  }

  if (get_lvgl_batch_depth_for_task(self) > 0) {
    auto *job = new LvglJobVoid<std::decay_t<F>>(std::forward<F>(fn));
    LvglJobBase *base = job;
    if (xQueueSend(g_lvgl_job_queue, &base, portMAX_DELAY) != pdTRUE) {
      ESP_LOGW(TAG, "lvgl_call_void: batched queue send failed, execute directly");
      job->run();
      delete job;
    }
    return;
  }

  LvglJobVoid<F> job(std::forward<F>(fn));
  job.waiter = xTaskGetCurrentTaskHandle();
  LvglJobBase *base = &job;
  if (xQueueSend(g_lvgl_job_queue, &base, portMAX_DELAY) != pdTRUE) {
    ESP_LOGW(TAG, "lvgl_call_void: queue send failed, execute directly");
    job.run();
    return;
  }
  wait_job_done(&job);
}

template<typename F> static auto lvgl_call_ret(F &&fn) -> decltype(fn()) {
  using R = decltype(fn());
  ensure_lvgl_job_queue();
  void *self = (void *) xTaskGetCurrentTaskHandle();
  if (g_lvgl_owner_task == nullptr || self == g_lvgl_owner_task || g_lvgl_job_queue == nullptr) {
    return fn();
  }

  if (get_lvgl_batch_depth_for_task(self) > 0) {
    flush_lvgl_jobs_for_current_task();
  }

  LvglJobRet<R, F> job(std::forward<F>(fn));
  job.waiter = xTaskGetCurrentTaskHandle();
  LvglJobBase *base = &job;
  if (xQueueSend(g_lvgl_job_queue, &base, portMAX_DELAY) != pdTRUE) {
    ESP_LOGW(TAG, "lvgl_call_ret: queue send failed, execute directly");
    job.run();
    return job.result;
  }
  wait_job_done(&job);
  return job.result;
}

static int l_batch_begin(lua_State *L) {
  (void) L;
  void *self = (void *) xTaskGetCurrentTaskHandle();
  int depth = get_lvgl_batch_depth_for_task(self);
  set_lvgl_batch_depth_for_task(self, depth + 1);
  return 0;
}

static int l_batch_end(lua_State *L) {
  (void) L;
  void *self = (void *) xTaskGetCurrentTaskHandle();
  int depth = get_lvgl_batch_depth_for_task(self);
  if (depth <= 0) return 0;
  depth--;
  set_lvgl_batch_depth_for_task(self, depth);
  if (depth == 0) {
    flush_lvgl_jobs_for_current_task();
  }
  return 0;
}

#define LUA_LVGL_IMPL
#include "lua_lvgl_gen.h"
#undef LUA_LVGL_IMPL

struct LuaEventCb {
  LuaLvglContext *ctx;
  int ref;
  int user_data_ref;
  int code_filter;
  lv_obj_t *obj;
  struct _lv_event_dsc_t *dsc;
};

struct LuaTimerCb {
  LuaLvglContext *ctx;
  int ref;
  int user_data_ref;
  lv_timer_t *timer;
  bool active;
  bool retired;
};

static void unref_lua_event_cb(lua_State *L, LuaEventCb *cb) {
  if (L == nullptr || cb == nullptr) return;
  if (cb->ref != LUA_NOREF) {
    luaL_unref(L, LUA_REGISTRYINDEX, cb->ref);
    cb->ref = LUA_NOREF;
  }
  if (cb->user_data_ref != LUA_NOREF) {
    luaL_unref(L, LUA_REGISTRYINDEX, cb->user_data_ref);
    cb->user_data_ref = LUA_NOREF;
  }
}

static void delete_lua_event_cb(lua_State *L, LuaEventCb *cb) {
  if (cb == nullptr) return;
  LuaLvglContext *ctx = cb->ctx;
  unref_lua_event_cb(L, cb);
  if (ctx != nullptr) {
    ctx->event_cbs.erase(cb);
  }
  cb->ctx = nullptr;
  cb->obj = nullptr;
  cb->dsc = nullptr;
  delete cb;
}

static void unref_lua_timer_cb(lua_State *L, LuaTimerCb *cb) {
  if (L == nullptr || cb == nullptr) return;
  if (cb->ref != LUA_NOREF) {
    luaL_unref(L, LUA_REGISTRYINDEX, cb->ref);
    cb->ref = LUA_NOREF;
  }
  if (cb->user_data_ref != LUA_NOREF) {
    luaL_unref(L, LUA_REGISTRYINDEX, cb->user_data_ref);
    cb->user_data_ref = LUA_NOREF;
  }
}

static void retire_lua_timer_cb(lua_State *L, LuaLvglContext *ctx, LuaTimerCb *cb) {
  if (cb == nullptr || cb->retired) return;
  cb->active = false;
  cb->retired = true;
  cb->timer = nullptr;
  unref_lua_timer_cb(L, cb);
  if (ctx != nullptr) {
    ctx->retired_timers.insert(cb);
  } else {
    cb->ctx = nullptr;
    delete cb;
  }
}

static void sweep_retired_lua_timer_cbs(LuaLvglContext *ctx) {
  if (ctx == nullptr || ctx->retired_timers.empty()) return;
  for (LuaTimerCb *cb : ctx->retired_timers) {
    if (cb == nullptr) continue;
    cb->ctx = nullptr;
    delete cb;
  }
  ctx->retired_timers.clear();
}

enum class LuaLvglMsgKind : int {
  EVENT = 0,
  TIMER = 1,
};

struct LuaLvglMsg {
  LuaLvglMsgKind kind;
  void *cb;
  void *target;
  int code;
  bool has_point;
  lv_point_t point;
  bool has_gesture_dir;
  lv_dir_t gesture_dir;
  bool has_key;
  uint32_t key;
};

static void push_lua_event(lua_State *L, lv_obj_t *target, int code, LuaEventCb *cb, const LuaLvglMsg *msg) {
  lua_createtable(L, 0, 5);

  lua_pushlightuserdata(L, target);
  lua_setfield(L, -2, "target");

  lua_pushinteger(L, code);
  lua_setfield(L, -2, "code");

  if (cb != nullptr && cb->user_data_ref != LUA_NOREF) {
    lua_rawgeti(L, LUA_REGISTRYINDEX, cb->user_data_ref);
  } else {
    lua_pushnil(L);
  }
  lua_setfield(L, -2, "user_data");

  if (msg != nullptr && msg->has_point) {
    lua_createtable(L, 0, 2);
    lua_pushinteger(L, (lua_Integer) msg->point.x);
    lua_setfield(L, -2, "x");
    lua_pushinteger(L, (lua_Integer) msg->point.y);
    lua_setfield(L, -2, "y");
    lua_setfield(L, -2, "point");
  } else {
    lua_pushnil(L);
    lua_setfield(L, -2, "point");
  }

  if (msg != nullptr && msg->has_gesture_dir) {
    lua_pushinteger(L, (lua_Integer) msg->gesture_dir);
    lua_setfield(L, -2, "gesture_dir");
  } else {
    lua_pushnil(L);
    lua_setfield(L, -2, "gesture_dir");
  }

  if (msg != nullptr && msg->has_key) {
    lua_pushinteger(L, (lua_Integer) msg->key);
    lua_setfield(L, -2, "key");
  } else {
    lua_pushnil(L);
    lua_setfield(L, -2, "key");
  }
}

static int l_event_get_code(lua_State *L) {
  luaL_checktype(L, 1, LUA_TTABLE);
  lua_getfield(L, 1, "code");
  return 1;
}

static int l_event_get_target(lua_State *L) {
  luaL_checktype(L, 1, LUA_TTABLE);
  lua_getfield(L, 1, "target");
  return 1;
}

static int l_event_get_user_data(lua_State *L) {
  luaL_checktype(L, 1, LUA_TTABLE);
  lua_getfield(L, 1, "user_data");
  return 1;
}

static int l_indev_get_act(lua_State *L) {
  lv_indev_t *indev = lvgl_call_ret([=]() -> lv_indev_t * { return lv_indev_get_act(); });
  lua_pushlightuserdata(L, (void *) indev);
  return 1;
}

static int l_indev_get_gesture_dir(lua_State *L) {
  const lv_indev_t *indev = (const lv_indev_t *) lua_touserdata(L, 1);
  if (indev == nullptr) {
    lua_pushnil(L);
    return 1;
  }
  lv_dir_t res = lvgl_call_ret([=]() -> lv_dir_t { return lv_indev_get_gesture_dir(indev); });
  lua_pushinteger(L, (lua_Integer) res);
  return 1;
}

static int l_indev_get_point(lua_State *L) {
  const lv_indev_t *indev = (const lv_indev_t *) lua_touserdata(L, 1);
  if (indev == nullptr) {
    lua_pushnil(L);
    return 1;
  }

  lv_point_t point = lvgl_call_ret([=]() -> lv_point_t {
    lv_point_t p{};
    lv_indev_get_point(indev, &p);
    return p;
  });

  lua_createtable(L, 0, 2);
  lua_pushinteger(L, (lua_Integer) point.x);
  lua_setfield(L, -2, "x");
  lua_pushinteger(L, (lua_Integer) point.y);
  lua_setfield(L, -2, "y");
  return 1;
}

static void lua_event_trampoline(lv_event_t *e) {
  auto *cb = static_cast<LuaEventCb *>(lv_event_get_user_data(e));
  if (cb == nullptr || cb->ctx == nullptr || !cb->ctx->alive || cb->ctx->queue == nullptr) return;
  if (cb->ref == LUA_NOREF) return;

  lv_event_code_t code = lv_event_get_code(e);
  if (code != LV_EVENT_DELETE && cb->code_filter != LV_EVENT_ALL && code != cb->code_filter) return;

  LuaLvglMsg msg{};
  msg.kind = LuaLvglMsgKind::EVENT;
  msg.cb = cb;
  msg.target = lv_event_get_target(e);
  msg.code = (int) code;
  msg.has_point = false;
  msg.point.x = 0;
  msg.point.y = 0;
  msg.has_gesture_dir = false;
  msg.gesture_dir = LV_DIR_NONE;
  msg.has_key = false;
  msg.key = 0;

  if (code == LV_EVENT_PRESSED || code == LV_EVENT_PRESSING || code == LV_EVENT_RELEASED ||
      code == LV_EVENT_CLICKED || code == LV_EVENT_SHORT_CLICKED || code == LV_EVENT_LONG_PRESSED ||
      code == LV_EVENT_LONG_PRESSED_REPEAT || code == LV_EVENT_GESTURE || code == LV_EVENT_KEY) {
    lv_indev_t *indev = lv_indev_get_act();
    if (indev != nullptr) {
      lv_indev_get_point(indev, &msg.point);
      msg.has_point = true;
      if (code == LV_EVENT_GESTURE) {
        msg.gesture_dir = lv_indev_get_gesture_dir(indev);
        msg.has_gesture_dir = true;
      }
      if (code == LV_EVENT_KEY) {
        msg.key = lv_indev_get_key(indev);
        msg.has_key = true;
      }
    }
  }

  xQueueSend(cb->ctx->queue, &msg, 0);
}

static void lua_timer_trampoline(lv_timer_t *timer) {
  auto *cb = static_cast<LuaTimerCb *>(timer != nullptr ? timer->user_data : nullptr);
  if (cb == nullptr || cb->ctx == nullptr || !cb->ctx->alive || cb->ctx->queue == nullptr) return;
  if (!cb->active || cb->ref == LUA_NOREF) return;

  LuaLvglMsg msg{LuaLvglMsgKind::TIMER, cb, timer, 0};
  xQueueSend(cb->ctx->queue, &msg, 0);
}

static int raise_callback_error(lua_State *L, LuaLvglContext *ctx, const char *prefix, const char *err) {
  std::string error_message = std::string(prefix) + ": " + (err ? err : "(unknown)");
  ESP_LOGE(TAG, "%s", error_message.c_str());

  if (ctx != nullptr && !ctx->fatal_error_reported) {
    ctx->fatal_error_reported = true;
    ctx->alive = false;
    show_lua_error_on_app_page(ctx->script_path, error_message);
  }

  return luaL_error(L, "%s", error_message.c_str());
}

static int l_poll_events(lua_State *L) {
  LuaLvglContext *ctx = get_lvgl_ctx(L);
  if (ctx == nullptr || !ctx->alive || ctx->queue == nullptr) {
    lua_pushinteger(L, 0);
    return 1;
  }

  int timeout = (int) luaL_optinteger(L, 1, 0);
  TickType_t ticks = (timeout < 0) ? portMAX_DELAY : pdMS_TO_TICKS(timeout);

  int processed = 0;
  LuaLvglMsg msg{};
  if (xQueueReceive(ctx->queue, &msg, ticks) == pdTRUE) {
    do {
      if (msg.kind == LuaLvglMsgKind::EVENT) {
        auto *cb = static_cast<LuaEventCb *>(msg.cb);
        auto *obj = static_cast<lv_obj_t *>(msg.target);
        if (cb != nullptr) {
          if (msg.code == LV_EVENT_DELETE) {
            delete_lua_event_cb(L, cb);
          } else if (cb->ref != LUA_NOREF) {
            lua_rawgeti(L, LUA_REGISTRYINDEX, cb->ref);
            push_lua_event(L, obj, msg.code, cb, &msg);
            if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
              const char *err = lua_tostring(L, -1);
              return raise_callback_error(L, ctx, "event cb error", err);
            }
          }
          processed++;
        }
      } else if (msg.kind == LuaLvglMsgKind::TIMER) {
        auto *cb = static_cast<LuaTimerCb *>(msg.cb);
        if (cb != nullptr && cb->active && cb->ref != LUA_NOREF) {
          lua_rawgeti(L, LUA_REGISTRYINDEX, cb->ref);
          lua_pushlightuserdata(L, msg.target);
          if (cb->user_data_ref != LUA_NOREF) {
            lua_rawgeti(L, LUA_REGISTRYINDEX, cb->user_data_ref);
          } else {
            lua_pushnil(L);
          }
          if (lua_pcall(L, 2, 0, 0) != LUA_OK) {
            const char *err = lua_tostring(L, -1);
            return raise_callback_error(L, ctx, "timer cb error", err);
          }
          processed++;
        }
      }
    } while (xQueueReceive(ctx->queue, &msg, 0) == pdTRUE);
  }

  sweep_retired_lua_timer_cbs(ctx);
  lua_pushinteger(L, processed);
  return 1;
}
static int l_obj_add_event_cb(lua_State *L) {
  lv_obj_t *obj = check_obj(L, 1);
  if (obj == nullptr) return 0;

  // Strict LVGL order: obj, cb, code[, user_data]
  luaL_checktype(L, 2, LUA_TFUNCTION);
  int code = LV_EVENT_ALL;
  if (lua_isnumber(L, 3)) code = (int) lua_tointeger(L, 3);

  LuaLvglContext *ctx = get_lvgl_ctx(L);
  if (ctx == nullptr || !ctx->alive) return 0;

  lua_pushvalue(L, 2);
  int ref = luaL_ref(L, LUA_REGISTRYINDEX);

  int user_data_ref = LUA_NOREF;
  if (lua_gettop(L) >= 4 && !lua_isnoneornil(L, 4)) {
    lua_pushvalue(L, 4);
    user_data_ref = luaL_ref(L, LUA_REGISTRYINDEX);
  }

  auto *cb = new LuaEventCb{ctx, ref, user_data_ref, code, obj, nullptr};
  struct _lv_event_dsc_t *dsc = lvgl_call_ret([=]() -> struct _lv_event_dsc_t * {
    return lv_obj_add_event_cb(obj, lua_event_trampoline, LV_EVENT_ALL, cb);
  });
  if (dsc == nullptr) {
    delete_lua_event_cb(L, cb);
    return 0;
  }
  cb->dsc = dsc;
  ctx->event_cbs.insert(cb);
  return 0;
}

static int l_btn_set_text(lua_State *L) {
  lv_obj_t *btn = check_obj(L, 1);
  const char *text = luaL_checkstring(L, 2);
  if (btn == nullptr) return 0;
  std::string text_copy = text != nullptr ? text : "";
  lvgl_call_void([=]() {
    lv_obj_t *label = lv_obj_get_child(btn, 0);
    if (label == nullptr) {
      label = lv_label_create(btn);
    }
    lv_label_set_text(label, text_copy.c_str());
    lv_obj_center(label);
  });
  return 0;
}

static int l_label_set_text_fmt(lua_State *L) {
  lv_obj_t *obj = check_obj(L, 1);
  if (obj == nullptr) return 0;

  int top = lua_gettop(L);
  if (top < 2) return 0;

  lua_getglobal(L, "string");
  lua_getfield(L, -1, "format");
  if (!lua_isfunction(L, -1)) {
    lua_pop(L, 2);
    const char *fallback = luaL_checkstring(L, 2);
    std::string fallback_text = fallback != nullptr ? fallback : "";
    lvgl_call_void([=]() { lv_label_set_text(obj, fallback_text.c_str()); });
    return 0;
  }

  lua_pushvalue(L, 2);
  for (int i = 3; i <= top; i++) lua_pushvalue(L, i);

  if (lua_pcall(L, top - 1, 1, 0) != LUA_OK) {
    const char *err = lua_tostring(L, -1);
    ESP_LOGE(TAG, "label_set_text_fmt error: %s", err ? err : "(unknown)");
    lua_pop(L, 2);
    const char *fallback = luaL_checkstring(L, 2);
    std::string fallback_text = fallback != nullptr ? fallback : "";
    lvgl_call_void([=]() { lv_label_set_text(obj, fallback_text.c_str()); });
    return 0;
  }

  const char *text = lua_tostring(L, -1);
  if (text != nullptr) {
    std::string text_copy = text;
    lvgl_call_void([=]() { lv_label_set_text(obj, text_copy.c_str()); });
  }
  lua_pop(L, 2);
  return 0;
}

static int l_dropdown_get_selected_str(lua_State *L) {
  lv_obj_t *obj = check_obj(L, 1);
  if (obj == nullptr) {
    lua_pushnil(L);
    return 1;
  }

  int size = (int) luaL_optinteger(L, 2, 64);
  if (size < 2) size = 2;
  if (size > 1024) size = 1024;

#if defined(LV_USE_DROPDOWN) && LV_USE_DROPDOWN
  std::string selected = lvgl_call_ret([=]() -> std::string {
    std::vector<char> buf((size_t) size, '\0');
    lv_dropdown_get_selected_str(obj, buf.data(), (uint32_t) buf.size());
    return std::string(buf.data());
  });
  lua_pushstring(L, selected.c_str());
#else
  lua_pushnil(L);
#endif
  return 1;
}

static int l_obj_center(lua_State *L) {
  lv_obj_t *obj = check_obj(L, 1);
  if (obj == nullptr) return 0;
  lvgl_call_void([=]() { lv_obj_center(obj); });
  return 0;
}

static bool has_case_insensitive_suffix(const std::string &value, const char *suffix) {
  size_t suffix_len = 0;
  while (suffix[suffix_len] != '\0') suffix_len++;
  if (value.size() < suffix_len) return false;

  size_t offset = value.size() - suffix_len;
  for (size_t i = 0; i < suffix_len; i++) {
    unsigned char lhs = static_cast<unsigned char>(value[offset + i]);
    unsigned char rhs = static_cast<unsigned char>(suffix[i]);
    if (std::tolower(lhs) != std::tolower(rhs)) return false;
  }
  return true;
}

static bool ends_with(const std::string &value, const std::string &suffix) {
  if (suffix.empty()) return true;
  if (value.size() < suffix.size()) return false;
  return value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

static bool has_drive_prefix(const std::string &path) {
  if (path.size() < 2 || path[1] != ':') return false;
  unsigned char drive = static_cast<unsigned char>(path[0]);
  return std::isalpha(drive) != 0;
}

static lv_disp_t *get_ui_disp_for_page(lv_obj_t *page) {
  if (page != nullptr) return lv_obj_get_disp(page);
  return lv_disp_get_default();
}

static lv_obj_t *get_ui_top_layer_for_page(lv_obj_t *page) {
  lv_disp_t *disp = get_ui_disp_for_page(page);
  if (disp == nullptr) return nullptr;
  return lv_disp_get_layer_top(disp);
}

struct LuaTopbarRefs {
  lv_obj_t *title{nullptr};
  lv_obj_t *date{nullptr};
  lv_obj_t *time{nullptr};
};

static LuaTopbarRefs find_topbar_refs(lv_obj_t *top_layer) {
  LuaTopbarRefs refs;
  if (top_layer == nullptr) return refs;

  lv_obj_t **slots[] = {&refs.title, &refs.date, &refs.time};
  size_t next_slot = 0;
  uint32_t child_count = lv_obj_get_child_cnt(top_layer);
  for (uint32_t i = 0; i < child_count && next_slot < 3; i++) {
    lv_obj_t *child = lv_obj_get_child(top_layer, i);
    if (child == nullptr || !lv_obj_check_type(child, &lv_label_class)) continue;
    *slots[next_slot] = child;
    next_slot++;
  }

  return refs;
}

static LuaNotificationBar *get_notification_bar_data(lv_obj_t *obj) {
  if (obj == nullptr) return nullptr;
  auto *data = static_cast<LuaNotificationBar *>(lv_obj_get_user_data(obj));
  if (data == nullptr || data->magic != LUA_NOTIFICATION_MAGIC) return nullptr;
  return data;
}

static lv_obj_t *get_notification_label(lv_obj_t *bar) {
  if (bar == nullptr) return nullptr;
  lv_obj_t *label = lv_obj_get_child(bar, 0);
  if (label == nullptr || !lv_obj_check_type(label, &lv_label_class)) return nullptr;
  return label;
}

static lv_obj_t *find_notification_bar(lv_obj_t *top_layer, const std::string &suffix) {
  if (top_layer == nullptr) return nullptr;
  uint32_t child_count = lv_obj_get_child_cnt(top_layer);
  for (uint32_t i = 0; i < child_count; i++) {
    lv_obj_t *child = lv_obj_get_child(top_layer, i);
    auto *data = get_notification_bar_data(child);
    if (data == nullptr || data->closing) continue;

    lv_obj_t *label = get_notification_label(child);
    if (label == nullptr) continue;
    const char *text = lv_label_get_text(label);
    if (text != nullptr && ends_with(text, suffix)) return child;
  }
  return nullptr;
}

static int count_notification_bars(lv_obj_t *top_layer) {
  if (top_layer == nullptr) return 0;
  int count = 0;
  uint32_t child_count = lv_obj_get_child_cnt(top_layer);
  for (uint32_t i = 0; i < child_count; i++) {
    if (get_notification_bar_data(lv_obj_get_child(top_layer, i)) != nullptr) count++;
  }
  return count;
}

static void notification_bar_delete_cb(lv_event_t *e) {
  lv_obj_t *bar = lv_event_get_target(e);
  auto *data = get_notification_bar_data(bar);
  if (data == nullptr) return;

  if (data->close_timer != nullptr) {
    lv_timer_del(data->close_timer);
    data->close_timer = nullptr;
  }
  data->bar = nullptr;
  data->closing = true;
  lv_obj_set_user_data(bar, nullptr);
  delete data;
}

static void notification_bar_hide_ready_cb(lv_anim_t *a) {
  lv_obj_t *bar = static_cast<lv_obj_t *>(a != nullptr ? a->var : nullptr);
  if (bar == nullptr || !lv_obj_is_valid(bar)) return;
  lv_obj_del(bar);
}

static void close_notification_bar(lv_obj_t *bar) {
  auto *data = get_notification_bar_data(bar);
  if (data == nullptr || data->closing) return;

  data->closing = true;
  if (data->close_timer != nullptr) {
    lv_timer_del(data->close_timer);
    data->close_timer = nullptr;
  }

  lv_anim_del(bar, (lv_anim_exec_xcb_t) lv_obj_set_y);

  lv_anim_t anim;
  lv_anim_init(&anim);
  lv_anim_set_var(&anim, bar);
  lv_anim_set_values(&anim, lv_obj_get_y(bar), NOTIFICATION_BAR_Y_HIDE);
  lv_anim_set_exec_cb(&anim, (lv_anim_exec_xcb_t) lv_obj_set_y);
  lv_anim_set_time(&anim, 300);
  lv_anim_set_path_cb(&anim, lv_anim_path_ease_in);
  lv_anim_set_ready_cb(&anim, notification_bar_hide_ready_cb);
  lv_anim_start(&anim);
}

static void notification_bar_timer_cb(lv_timer_t *timer) {
  auto *data = static_cast<LuaNotificationBar *>(timer != nullptr ? timer->user_data : nullptr);
  if (data == nullptr || data->magic != LUA_NOTIFICATION_MAGIC) return;
  data->close_timer = nullptr;
  if (data->bar == nullptr || !lv_obj_is_valid(data->bar)) return;
  close_notification_bar(data->bar);
}

static void notification_bar_click_cb(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
  close_notification_bar(lv_event_get_target(e));
}

static void set_notification_bar_timer(LuaNotificationBar *data, int delay_ms) {
  if (data == nullptr) return;
  if (data->close_timer != nullptr) {
    lv_timer_del(data->close_timer);
    data->close_timer = nullptr;
  }
  if (delay_ms <= 0) return;

  data->close_timer = lv_timer_create(notification_bar_timer_cb, (uint32_t) delay_ms, data);
  if (data->close_timer != nullptr) {
    lv_timer_set_repeat_count(data->close_timer, 1);
  }
}

static int l_ui_hide_topbar(lua_State *L) {
  lv_obj_t *app_page = get_app_page_from_lua(L);
  lvgl_call_void([app_page]() {
    LuaTopbarRefs refs = find_topbar_refs(get_ui_top_layer_for_page(app_page));
    if (refs.title != nullptr) lv_obj_add_flag(refs.title, LV_OBJ_FLAG_HIDDEN);
    if (refs.date != nullptr) lv_obj_add_flag(refs.date, LV_OBJ_FLAG_HIDDEN);
    if (refs.time != nullptr) lv_obj_add_flag(refs.time, LV_OBJ_FLAG_HIDDEN);
  });
  return 0;
}

static int l_ui_show_topbar(lua_State *L) {
  lv_obj_t *app_page = get_app_page_from_lua(L);
  lvgl_call_void([app_page]() {
    LuaTopbarRefs refs = find_topbar_refs(get_ui_top_layer_for_page(app_page));
    if (refs.title != nullptr) lv_obj_clear_flag(refs.title, LV_OBJ_FLAG_HIDDEN);
    if (refs.date != nullptr) lv_obj_clear_flag(refs.date, LV_OBJ_FLAG_HIDDEN);
    if (refs.time != nullptr) lv_obj_clear_flag(refs.time, LV_OBJ_FLAG_HIDDEN);
  });
  return 0;
}

static int l_ui_show_notification(lua_State *L) {
  std::string message = luaL_checkstring(L, 1);
  std::string suffix = luaL_optstring(L, 2, "");
  int delay_ms = (int) luaL_optinteger(L, 3, 0);
  lv_obj_t *app_page = get_app_page_from_lua(L);

  lvgl_call_void([app_page, message, suffix, delay_ms]() {
    lv_obj_t *top_layer = get_ui_top_layer_for_page(app_page);
    if (top_layer == nullptr) return;

    lv_obj_t *bar = suffix.empty() ? nullptr : find_notification_bar(top_layer, suffix);
    if (bar != nullptr) {
      auto *data = get_notification_bar_data(bar);
      lv_obj_t *label = get_notification_label(bar);
      if (data != nullptr && label != nullptr) {
        data->closing = false;
        lv_anim_del(bar, (lv_anim_exec_xcb_t) lv_obj_set_y);
        lv_obj_move_foreground(bar);
        lv_label_set_text(label, message.c_str());
        lv_obj_set_y(bar, NOTIFICATION_BAR_Y_SHOW);

        lv_anim_t shake_anim;
        lv_anim_init(&shake_anim);
        lv_anim_set_var(&shake_anim, bar);
        lv_anim_set_values(&shake_anim, NOTIFICATION_BAR_Y_SHOW, NOTIFICATION_BAR_Y_SHOW + 8);
        lv_anim_set_exec_cb(&shake_anim, (lv_anim_exec_xcb_t) lv_obj_set_y);
        lv_anim_set_time(&shake_anim, 100);
        lv_anim_set_playback_time(&shake_anim, 100);
        lv_anim_start(&shake_anim);

        set_notification_bar_timer(data, delay_ms);
      }
      return;
    }

    if (count_notification_bars(top_layer) >= 15) {
      ESP_LOGW(TAG, "Reached notification limit, skip showing new notification");
      return;
    }

    bar = lv_obj_create(top_layer);
    lv_obj_set_size(bar, 240, 25);
    lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, NOTIFICATION_BAR_Y_HIDE);
    lv_obj_set_scrollbar_mode(bar, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_bg_opa(bar, 175, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);

    auto *data = new LuaNotificationBar{LUA_NOTIFICATION_MAGIC, bar, nullptr, false};
    lv_obj_set_user_data(bar, data);
    lv_obj_add_event_cb(bar, notification_bar_delete_cb, LV_EVENT_DELETE, nullptr);
    lv_obj_add_event_cb(bar, notification_bar_click_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *label = lv_label_create(bar);
    lv_label_set_text(label, message.c_str());
    lv_obj_set_width(label, 220);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);

    lv_anim_t in_anim;
    lv_anim_init(&in_anim);
    lv_anim_set_var(&in_anim, bar);
    lv_anim_set_values(&in_anim, NOTIFICATION_BAR_Y_HIDE, NOTIFICATION_BAR_Y_SHOW);
    lv_anim_set_exec_cb(&in_anim, (lv_anim_exec_xcb_t) lv_obj_set_y);
    lv_anim_set_time(&in_anim, 500);
    lv_anim_set_path_cb(&in_anim, lv_anim_path_overshoot);
    lv_anim_start(&in_anim);

    set_notification_bar_timer(data, delay_ms);
  });

  return 0;
}

static int l_ui_close_notification(lua_State *L) {
  const char *suffix = luaL_optstring(L, 1, "");
  std::string suffix_str = suffix ? suffix : "";
  lv_obj_t *app_page = get_app_page_from_lua(L);

  lvgl_call_void([app_page, suffix_str]() {
    lv_obj_t *top_layer = get_ui_top_layer_for_page(app_page);
    if (top_layer == nullptr) return;

    if (suffix_str.empty()) {
      std::vector<lv_obj_t *> bars;
      uint32_t child_count = lv_obj_get_child_cnt(top_layer);
      for (uint32_t i = 0; i < child_count; i++) {
        lv_obj_t *child = lv_obj_get_child(top_layer, i);
        if (get_notification_bar_data(child) != nullptr) bars.push_back(child);
      }
      for (lv_obj_t *bar : bars) close_notification_bar(bar);
      return;
    }

    lv_obj_t *bar = find_notification_bar(top_layer, suffix_str);
    if (bar != nullptr) {
      close_notification_bar(bar);
    } else {
      ESP_LOGW(TAG, "No matching notification found for suffix: %s", suffix_str.c_str());
    }
  });

  return 0;
}

void show_lua_error_on_app_page(const std::string &script_path, const std::string &message) {
  std::string app_dir = dir_from_path(script_path);
  lv_obj_t *page = get_app_page(app_dir);
  if (page == nullptr) return;

  std::string short_msg = shorten_error_message(message, 220);
  lvgl_call_void([page, short_msg]() {
    if (!lv_obj_is_valid(page)) return;

    lv_obj_clean(page);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(page, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_grad_color(page, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_grad_dir(page, LV_GRAD_DIR_NONE, 0);
    lv_obj_set_style_bg_main_stop(page, 255, 0);
    lv_obj_set_style_bg_grad_stop(page, 255, 0);
    lv_obj_set_style_bg_opa(page, LV_OPA_COVER, 0);

    lv_obj_t *title = lv_label_create(page);
    lv_label_set_text(title, "Lua 脚本错误");
    lv_obj_set_style_text_color(title, lv_color_hex(0xCC3333), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 28);

    lv_obj_t *detail = lv_label_create(page);
    lv_obj_set_width(detail, 280);
    lv_obj_set_style_text_align(detail, LV_TEXT_ALIGN_LEFT, 0);
    std::string detail_text = short_msg + "\n\n脚本已停止运行。请按 Home 键返回桌面。";
    lv_label_set_text(detail, detail_text.c_str());
    lv_obj_set_style_text_color(detail, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(detail, LV_ALIGN_TOP_LEFT, 20, 68);
  });
}

static std::string join_path(const std::string &base, const std::string &relative) {
  if (base.empty() || base == ".") return relative;
  if (base == "/") return "/" + relative;
  if (!base.empty() && base.back() == '/') return base + relative;
  return base + "/" + relative;
}

static std::string normalize_lvgl_fs_path(lua_State *L, const std::string &path) {
  if (path.empty()) return path;
  if (has_drive_prefix(path)) return path;
  if (path[0] == '/') return std::string(1, LV_FS_POSIX_LETTER) + ":" + path;

  std::string script_dir = get_script_dir(L);
  std::string resolved = join_path(script_dir, path);
  if (has_drive_prefix(resolved)) return resolved;
  if (!resolved.empty() && resolved[0] == '/') return std::string(1, LV_FS_POSIX_LETTER) + ":" + resolved;
  return std::string(1, LV_FS_POSIX_LETTER) + ":/" + resolved;
}

static int l_img_set_src(lua_State *L) {
  lv_obj_t *obj = check_obj(L, 1);
  if (obj == nullptr) return 0;

  const void *src = nullptr;
  std::string normalized_path;
  if (lua_isstring(L, 2)) {
    normalized_path = normalize_lvgl_fs_path(L, lua_tostring(L, 2));
    src = normalized_path.c_str();
  } else if (lua_islightuserdata(L, 2)) {
    src = lua_touserdata(L, 2);
  }

  if (src == nullptr) return 0;
  lvgl_call_void([=]() {
    const void *resolved_src = normalized_path.empty() ? src : normalized_path.c_str();
    lv_img_set_src(obj, resolved_src);
  });
  return 0;
}

static int l_font_load(lua_State *L) {
  const char *path = luaL_checkstring(L, 1);
  std::string path_str = normalize_lvgl_fs_path(L, path);
  bool is_bin_font = has_case_insensitive_suffix(path_str, ".bin");

  LuaLvglContext *ctx = get_lvgl_ctx(L);
  if (ctx == nullptr || !ctx->alive) {
    lua_pushnil(L);
    return 1;
  }

  lv_font_t *font = nullptr;
  LuaFontKind kind = LuaFontKind::BIN;
  if (is_bin_font) {
    font = lvgl_call_ret([=]() -> lv_font_t * { return lv_font_load(path_str.c_str()); });
  } else {
    lv_coord_t font_size = (lv_coord_t) luaL_checkinteger(L, 2);
    size_t cache_size = (size_t) luaL_optinteger(L, 3, 4096);
#if LV_USE_TINY_TTF && LV_TINY_TTF_FILE_SUPPORT
    font = lvgl_call_ret([=]() -> lv_font_t * {
      return lv_tiny_ttf_create_file_ex(path_str.c_str(), font_size, cache_size);
    });
    kind = LuaFontKind::TINY_TTF;
#else
    (void) font_size;
    (void) cache_size;
#endif
  }

  if (font == nullptr) {
    lua_pushnil(L);
    return 1;
  }

  ctx->fonts[font] = kind;
  lua_pushlightuserdata(L, (void *) font);
  return 1;
}

static int l_obj_set_style_pad_all(lua_State *L) {
  lv_obj_t *obj = check_obj(L, 1);
  lv_coord_t value = (lv_coord_t) luaL_checkinteger(L, 2);
  lv_style_selector_t selector = (lv_style_selector_t) luaL_checkinteger(L, 3);
  if (obj == nullptr) return 0;

  lvgl_call_void([=]() { lv_obj_set_style_pad_all(obj, value, selector); });
  return 0;
}

static int l_obj_set_style_pad_hor(lua_State *L) {
  lv_obj_t *obj = check_obj(L, 1);
  lv_coord_t value = (lv_coord_t) luaL_checkinteger(L, 2);
  lv_style_selector_t selector = (lv_style_selector_t) luaL_checkinteger(L, 3);
  if (obj == nullptr) return 0;

  lvgl_call_void([=]() { lv_obj_set_style_pad_hor(obj, value, selector); });
  return 0;
}

static int l_obj_set_style_pad_ver(lua_State *L) {
  lv_obj_t *obj = check_obj(L, 1);
  lv_coord_t value = (lv_coord_t) luaL_checkinteger(L, 2);
  lv_style_selector_t selector = (lv_style_selector_t) luaL_checkinteger(L, 3);
  if (obj == nullptr) return 0;

  lvgl_call_void([=]() { lv_obj_set_style_pad_ver(obj, value, selector); });
  return 0;
}

static int l_obj_set_style_pad_gap(lua_State *L) {
  lv_obj_t *obj = check_obj(L, 1);
  lv_coord_t value = (lv_coord_t) luaL_checkinteger(L, 2);
  lv_style_selector_t selector = (lv_style_selector_t) luaL_checkinteger(L, 3);
  if (obj == nullptr) return 0;

  lvgl_call_void([=]() { lv_obj_set_style_pad_gap(obj, value, selector); });
  return 0;
}

static int l_obj_set_style_size(lua_State *L) {
  lv_obj_t *obj = check_obj(L, 1);
  lv_coord_t value = (lv_coord_t) luaL_checkinteger(L, 2);
  lv_style_selector_t selector = (lv_style_selector_t) luaL_checkinteger(L, 3);
  if (obj == nullptr) return 0;

  lvgl_call_void([=]() { lv_obj_set_style_size(obj, value, selector); });
  return 0;
}

static int l_font_free(lua_State *L) {
  lv_font_t *font = (lv_font_t *) lua_touserdata(L, 1);
  if (font == nullptr) return 0;

  LuaLvglContext *ctx = get_lvgl_ctx(L);
  LuaFontKind kind = LuaFontKind::BIN;
  bool owned = false;
  if (ctx != nullptr) {
    auto it = ctx->fonts.find(font);
    if (it != ctx->fonts.end()) {
      kind = it->second;
      ctx->fonts.erase(it);
      owned = true;
    }
  }
  if (!owned) return 0;

  lvgl_call_void([=]() {
    switch (kind) {
      case LuaFontKind::BIN:
        lv_font_free(font);
        break;
      case LuaFontKind::TINY_TTF:
#if LV_USE_TINY_TTF
        lv_tiny_ttf_destroy(font);
#endif
        break;
    }
  });
  return 0;
}

static int l_timer_create(lua_State *L) {
  luaL_checktype(L, 1, LUA_TFUNCTION);
  uint32_t period = (uint32_t) luaL_checkinteger(L, 2);

  LuaLvglContext *ctx = get_lvgl_ctx(L);
  if (ctx == nullptr || !ctx->alive) {
    lua_pushnil(L);
    return 1;
  }

  auto *cb = new LuaTimerCb{ctx, LUA_NOREF, LUA_NOREF, nullptr, true, false};
  lua_pushvalue(L, 1);
  cb->ref = luaL_ref(L, LUA_REGISTRYINDEX);
  if (!lua_isnoneornil(L, 3)) {
    lua_pushvalue(L, 3);
    cb->user_data_ref = luaL_ref(L, LUA_REGISTRYINDEX);
  }

  lv_timer_t *timer = lvgl_call_ret([=]() -> lv_timer_t * {
    return lv_timer_create(lua_timer_trampoline, period, cb);
  });
  if (timer == nullptr) {
    if (cb->ref != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, cb->ref);
    if (cb->user_data_ref != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, cb->user_data_ref);
    delete cb;
    lua_pushnil(L);
    return 1;
  }

  cb->timer = timer;
  ctx->timers[timer] = cb;
  lua_pushlightuserdata(L, (void *) timer);
  return 1;
}

static int l_timer_del(lua_State *L) {
  lv_timer_t *timer = (lv_timer_t *) lua_touserdata(L, 1);
  if (timer == nullptr) return 0;

  LuaLvglContext *ctx = get_lvgl_ctx(L);
  if (ctx != nullptr) {
    auto it = ctx->timers.find(timer);
    if (it != ctx->timers.end()) {
      LuaTimerCb *cb = it->second;
      ctx->timers.erase(it);
      if (cb != nullptr) {
        retire_lua_timer_cb(L, ctx, cb);
      }
    }
  }

  lvgl_call_void([=]() {
    timer->user_data = nullptr;
    lv_timer_del(timer);
  });
  return 0;
}

static int l_btnmatrix_get_selected_btn(lua_State *L) {
  const lv_obj_t *obj = check_obj(L, 1);
  if (obj == nullptr) {
    lua_pushnil(L);
    return 1;
  }
  uint16_t res = lvgl_call_ret([=]() -> uint16_t {
    return lv_btnmatrix_get_selected_btn(obj);
  });
  lua_pushinteger(L, (lua_Integer) res);
  return 1;
}

static int l_btnmatrix_get_btn_text(lua_State *L) {
  const lv_obj_t *obj = check_obj(L, 1);
  uint16_t btn_id = (uint16_t) luaL_checkinteger(L, 2);
  if (obj == nullptr) {
    lua_pushnil(L);
    return 1;
  }
  const char *res = lvgl_call_ret([=]() -> const char * {
    return lv_btnmatrix_get_btn_text(obj, btn_id);
  });
  if (res) {
    lua_pushstring(L, res);
  } else {
    lua_pushnil(L);
  }
  return 1;
}

static int l_btnmatrix_get_map(lua_State *L) {
  const lv_obj_t *obj = check_obj(L, 1);
  if (obj == nullptr) {
    lua_pushnil(L);
    return 1;
  }
  const char **map = lvgl_call_ret([=]() -> const char ** {
    return lv_btnmatrix_get_map(obj);
  });
  if (map == nullptr) {
    lua_pushnil(L);
    return 1;
  }
  lua_newtable(L);
  int idx = 1;
  for (int i = 0; map[i] != nullptr; i++) {
    lua_pushstring(L, map[i]);
    lua_seti(L, -2, idx++);
  }
  return 1;
}

static void set_int_field(lua_State *L, const char *name, int value) {
  lua_pushinteger(L, value);
  lua_setfield(L, -2, name);
}

void register_lvgl_api(lua_State *L, const std::string &script_path) {
  std::string app_dir = dir_from_path(script_path);
  lv_obj_t *page = get_app_page(app_dir);
  set_app_page(L, page);
  set_script_dir(L, app_dir);

  auto *ctx = new LuaLvglContext;
  ctx->queue = xQueueCreate(16, sizeof(LuaLvglMsg));
  ctx->alive = true;
  ctx->script_path = script_path;
  set_lvgl_ctx(L, ctx);

  lua_newtable(L);  // lvgl
  lua_pushcfunction(L, l_app_page);
  lua_setfield(L, -2, "app_page");
  lua_pushcfunction(L, l_poll_events);
  lua_setfield(L, -2, "poll_events");
  lua_pushcfunction(L, l_batch_begin);
  lua_setfield(L, -2, "batch_begin");
  lua_pushcfunction(L, l_batch_end);
  lua_setfield(L, -2, "batch_end");

  register_lvgl_gen(L);

  lua_pushcfunction(L, l_obj_add_event_cb);
  lua_setfield(L, -2, "obj_add_event_cb");
  lua_pushcfunction(L, l_event_get_code);
  lua_setfield(L, -2, "event_get_code");
  lua_pushcfunction(L, l_event_get_target);
  lua_setfield(L, -2, "event_get_target");
  lua_pushcfunction(L, l_event_get_user_data);
  lua_setfield(L, -2, "event_get_user_data");
  lua_pushcfunction(L, l_indev_get_act);
  lua_setfield(L, -2, "indev_get_act");
  lua_pushcfunction(L, l_indev_get_gesture_dir);
  lua_setfield(L, -2, "indev_get_gesture_dir");
  lua_pushcfunction(L, l_indev_get_point);
  lua_setfield(L, -2, "indev_get_point");
  lua_pushcfunction(L, l_btn_set_text);
  lua_setfield(L, -2, "btn_set_text");
  lua_pushcfunction(L, l_label_set_text_fmt);
  lua_setfield(L, -2, "label_set_text_fmt");
  lua_pushcfunction(L, l_dropdown_get_selected_str);
  lua_setfield(L, -2, "dropdown_get_selected_str");
  lua_pushcfunction(L, l_obj_center);
  lua_setfield(L, -2, "obj_center");
  lua_pushcfunction(L, l_img_set_src);
  lua_setfield(L, -2, "img_set_src");
  lua_pushcfunction(L, l_obj_set_style_pad_all);
  lua_setfield(L, -2, "obj_set_style_pad_all");
  lua_pushcfunction(L, l_obj_set_style_pad_hor);
  lua_setfield(L, -2, "obj_set_style_pad_hor");
  lua_pushcfunction(L, l_obj_set_style_pad_ver);
  lua_setfield(L, -2, "obj_set_style_pad_ver");
  lua_pushcfunction(L, l_obj_set_style_pad_gap);
  lua_setfield(L, -2, "obj_set_style_pad_gap");
  lua_pushcfunction(L, l_obj_set_style_size);
  lua_setfield(L, -2, "obj_set_style_size");
  lua_pushcfunction(L, l_font_load);
  lua_setfield(L, -2, "font_load");
  lua_pushcfunction(L, l_font_free);
  lua_setfield(L, -2, "font_free");
  lua_pushcfunction(L, l_timer_create);
  lua_setfield(L, -2, "timer_create");
  lua_pushcfunction(L, l_timer_del);
  lua_setfield(L, -2, "timer_del");
  lua_pushcfunction(L, l_btnmatrix_get_selected_btn);
  lua_setfield(L, -2, "btnmatrix_get_selected_btn");
  lua_pushcfunction(L, l_btnmatrix_get_btn_text);
  lua_setfield(L, -2, "btnmatrix_get_btn_text");
  lua_pushcfunction(L, l_btnmatrix_get_map);
  lua_setfield(L, -2, "btnmatrix_get_map");
  // align constants
  set_int_field(L, "ALIGN_CENTER", LV_ALIGN_CENTER);
  set_int_field(L, "ALIGN_TOP_LEFT", LV_ALIGN_TOP_LEFT);
  set_int_field(L, "ALIGN_TOP_MID", LV_ALIGN_TOP_MID);
  set_int_field(L, "ALIGN_TOP_RIGHT", LV_ALIGN_TOP_RIGHT);
  set_int_field(L, "ALIGN_LEFT_MID", LV_ALIGN_LEFT_MID);
  set_int_field(L, "ALIGN_RIGHT_MID", LV_ALIGN_RIGHT_MID);
  set_int_field(L, "ALIGN_BOTTOM_LEFT", LV_ALIGN_BOTTOM_LEFT);
  set_int_field(L, "ALIGN_BOTTOM_MID", LV_ALIGN_BOTTOM_MID);
  set_int_field(L, "ALIGN_BOTTOM_RIGHT", LV_ALIGN_BOTTOM_RIGHT);

  // flags
  set_int_field(L, "FLAG_HIDDEN", LV_OBJ_FLAG_HIDDEN);
  set_int_field(L, "FLAG_CLICKABLE", LV_OBJ_FLAG_CLICKABLE);
  set_int_field(L, "FLAG_CLICK_FOCUSABLE", LV_OBJ_FLAG_CLICK_FOCUSABLE);
  set_int_field(L, "FLAG_CHECKABLE", LV_OBJ_FLAG_CHECKABLE);
  set_int_field(L, "FLAG_SCROLLABLE", LV_OBJ_FLAG_SCROLLABLE);
  set_int_field(L, "FLAG_SCROLL_ELASTIC", LV_OBJ_FLAG_SCROLL_ELASTIC);
  set_int_field(L, "FLAG_SCROLL_MOMENTUM", LV_OBJ_FLAG_SCROLL_MOMENTUM);
  set_int_field(L, "FLAG_SCROLL_ONE", LV_OBJ_FLAG_SCROLL_ONE);
  set_int_field(L, "FLAG_SCROLL_CHAIN_HOR", LV_OBJ_FLAG_SCROLL_CHAIN_HOR);
  set_int_field(L, "FLAG_SCROLL_CHAIN_VER", LV_OBJ_FLAG_SCROLL_CHAIN_VER);
  set_int_field(L, "FLAG_SCROLL_CHAIN", LV_OBJ_FLAG_SCROLL_CHAIN);
  set_int_field(L, "FLAG_SCROLL_ON_FOCUS", LV_OBJ_FLAG_SCROLL_ON_FOCUS);
  set_int_field(L, "FLAG_SCROLL_WITH_ARROW", LV_OBJ_FLAG_SCROLL_WITH_ARROW);
  set_int_field(L, "FLAG_SNAPPABLE", LV_OBJ_FLAG_SNAPPABLE);
  set_int_field(L, "FLAG_PRESS_LOCK", LV_OBJ_FLAG_PRESS_LOCK);
  set_int_field(L, "FLAG_EVENT_BUBBLE", LV_OBJ_FLAG_EVENT_BUBBLE);
  set_int_field(L, "FLAG_GESTURE_BUBBLE", LV_OBJ_FLAG_GESTURE_BUBBLE);
  set_int_field(L, "FLAG_ADV_HITTEST", LV_OBJ_FLAG_ADV_HITTEST);
  set_int_field(L, "FLAG_IGNORE_LAYOUT", LV_OBJ_FLAG_IGNORE_LAYOUT);
  set_int_field(L, "FLAG_FLOATING", LV_OBJ_FLAG_FLOATING);
  set_int_field(L, "FLAG_OVERFLOW_VISIBLE", LV_OBJ_FLAG_OVERFLOW_VISIBLE);
  set_int_field(L, "FLAG_LAYOUT_1", LV_OBJ_FLAG_LAYOUT_1);
  set_int_field(L, "FLAG_LAYOUT_2", LV_OBJ_FLAG_LAYOUT_2);
  set_int_field(L, "FLAG_WIDGET_1", LV_OBJ_FLAG_WIDGET_1);
  set_int_field(L, "FLAG_WIDGET_2", LV_OBJ_FLAG_WIDGET_2);
  set_int_field(L, "FLAG_USER_1", LV_OBJ_FLAG_USER_1);
  set_int_field(L, "FLAG_USER_2", LV_OBJ_FLAG_USER_2);
  set_int_field(L, "FLAG_USER_3", LV_OBJ_FLAG_USER_3);
  set_int_field(L, "FLAG_USER_4", LV_OBJ_FLAG_USER_4);

  // state
  set_int_field(L, "STATE_DEFAULT", LV_STATE_DEFAULT);
  set_int_field(L, "STATE_CHECKED", LV_STATE_CHECKED);
  set_int_field(L, "STATE_FOCUSED", LV_STATE_FOCUSED);
  set_int_field(L, "STATE_PRESSED", LV_STATE_PRESSED);
  set_int_field(L, "STATE_DISABLED", LV_STATE_DISABLED);

  // part
  set_int_field(L, "PART_MAIN", LV_PART_MAIN);
  set_int_field(L, "PART_SCROLLBAR", LV_PART_SCROLLBAR);
  set_int_field(L, "PART_INDICATOR", LV_PART_INDICATOR);
  set_int_field(L, "PART_ITEMS", LV_PART_ITEMS);
  set_int_field(L, "PART_KNOB", LV_PART_KNOB);

  // anim
  set_int_field(L, "ANIM_OFF", LV_ANIM_OFF);
  set_int_field(L, "ANIM_ON", LV_ANIM_ON);

  // dir
  set_int_field(L, "DIR_NONE", LV_DIR_NONE);
  set_int_field(L, "DIR_LEFT", LV_DIR_LEFT);
  set_int_field(L, "DIR_RIGHT", LV_DIR_RIGHT);
  set_int_field(L, "DIR_TOP", LV_DIR_TOP);
  set_int_field(L, "DIR_BOTTOM", LV_DIR_BOTTOM);
  set_int_field(L, "DIR_HOR", LV_DIR_HOR);
  set_int_field(L, "DIR_VER", LV_DIR_VER);
  set_int_field(L, "DIR_ALL", LV_DIR_ALL);

  // layout
  set_int_field(L, "LAYOUT_FLEX", LV_LAYOUT_FLEX);

  // flex flow
  set_int_field(L, "FLEX_FLOW_ROW", LV_FLEX_FLOW_ROW);
  set_int_field(L, "FLEX_FLOW_COLUMN", LV_FLEX_FLOW_COLUMN);
  set_int_field(L, "FLEX_FLOW_ROW_WRAP", LV_FLEX_FLOW_ROW_WRAP);
  set_int_field(L, "FLEX_FLOW_ROW_REVERSE", LV_FLEX_FLOW_ROW_REVERSE);
  set_int_field(L, "FLEX_FLOW_ROW_WRAP_REVERSE", LV_FLEX_FLOW_ROW_WRAP_REVERSE);
  set_int_field(L, "FLEX_FLOW_COLUMN_WRAP", LV_FLEX_FLOW_COLUMN_WRAP);
  set_int_field(L, "FLEX_FLOW_COLUMN_REVERSE", LV_FLEX_FLOW_COLUMN_REVERSE);
  set_int_field(L, "FLEX_FLOW_COLUMN_WRAP_REVERSE", LV_FLEX_FLOW_COLUMN_WRAP_REVERSE);

  // flex align
  set_int_field(L, "FLEX_ALIGN_START", LV_FLEX_ALIGN_START);
  set_int_field(L, "FLEX_ALIGN_END", LV_FLEX_ALIGN_END);
  set_int_field(L, "FLEX_ALIGN_CENTER", LV_FLEX_ALIGN_CENTER);
  set_int_field(L, "FLEX_ALIGN_SPACE_EVENLY", LV_FLEX_ALIGN_SPACE_EVENLY);
  set_int_field(L, "FLEX_ALIGN_SPACE_AROUND", LV_FLEX_ALIGN_SPACE_AROUND);
  set_int_field(L, "FLEX_ALIGN_SPACE_BETWEEN", LV_FLEX_ALIGN_SPACE_BETWEEN);

  // opacity
  set_int_field(L, "OPA_TRANSP", LV_OPA_TRANSP);
  set_int_field(L, "OPA_COVER", LV_OPA_COVER);

  // gradient
  set_int_field(L, "GRAD_DIR_NONE", LV_GRAD_DIR_NONE);
  set_int_field(L, "GRAD_DIR_VER", LV_GRAD_DIR_VER);
  set_int_field(L, "GRAD_DIR_HOR", LV_GRAD_DIR_HOR);

  // dither
  set_int_field(L, "DITHER_NONE", LV_DITHER_NONE);
  set_int_field(L, "DITHER_ORDERED", LV_DITHER_ORDERED);
  set_int_field(L, "DITHER_ERR_DIFF", LV_DITHER_ERR_DIFF);

  // border side
  set_int_field(L, "BORDER_SIDE_NONE", LV_BORDER_SIDE_NONE);
  set_int_field(L, "BORDER_SIDE_BOTTOM", LV_BORDER_SIDE_BOTTOM);
  set_int_field(L, "BORDER_SIDE_TOP", LV_BORDER_SIDE_TOP);
  set_int_field(L, "BORDER_SIDE_LEFT", LV_BORDER_SIDE_LEFT);
  set_int_field(L, "BORDER_SIDE_RIGHT", LV_BORDER_SIDE_RIGHT);
  set_int_field(L, "BORDER_SIDE_FULL", LV_BORDER_SIDE_FULL);
  set_int_field(L, "BORDER_SIDE_INTERNAL", LV_BORDER_SIDE_INTERNAL);

  // text decor
  set_int_field(L, "TEXT_DECOR_NONE", LV_TEXT_DECOR_NONE);
  set_int_field(L, "TEXT_DECOR_UNDERLINE", LV_TEXT_DECOR_UNDERLINE);
  set_int_field(L, "TEXT_DECOR_STRIKETHROUGH", LV_TEXT_DECOR_STRIKETHROUGH);

  // base dir
  set_int_field(L, "BASE_DIR_LTR", LV_BASE_DIR_LTR);
  set_int_field(L, "BASE_DIR_RTL", LV_BASE_DIR_RTL);
  set_int_field(L, "BASE_DIR_AUTO", LV_BASE_DIR_AUTO);
  set_int_field(L, "BASE_DIR_NEUTRAL", LV_BASE_DIR_NEUTRAL);
  set_int_field(L, "BASE_DIR_WEAK", LV_BASE_DIR_WEAK);

  // blend mode
  set_int_field(L, "BLEND_MODE_NORMAL", LV_BLEND_MODE_NORMAL);
  set_int_field(L, "BLEND_MODE_ADDITIVE", LV_BLEND_MODE_ADDITIVE);
  set_int_field(L, "BLEND_MODE_SUBTRACTIVE", LV_BLEND_MODE_SUBTRACTIVE);
  set_int_field(L, "BLEND_MODE_MULTIPLY", LV_BLEND_MODE_MULTIPLY);
  set_int_field(L, "BLEND_MODE_REPLACE", LV_BLEND_MODE_REPLACE);

  // text align
  set_int_field(L, "TEXT_ALIGN_LEFT", LV_TEXT_ALIGN_LEFT);
  set_int_field(L, "TEXT_ALIGN_CENTER", LV_TEXT_ALIGN_CENTER);
  set_int_field(L, "TEXT_ALIGN_RIGHT", LV_TEXT_ALIGN_RIGHT);
  set_int_field(L, "TEXT_ALIGN_AUTO", LV_TEXT_ALIGN_AUTO);

  // events
  set_int_field(L, "EVENT_ALL", LV_EVENT_ALL);
  set_int_field(L, "EVENT_PRESSED", LV_EVENT_PRESSED);
  set_int_field(L, "EVENT_PRESSING", LV_EVENT_PRESSING);
  set_int_field(L, "EVENT_RELEASED", LV_EVENT_RELEASED);
  set_int_field(L, "EVENT_CLICKED", LV_EVENT_CLICKED);
  set_int_field(L, "EVENT_SHORT_CLICKED", LV_EVENT_SHORT_CLICKED);
  set_int_field(L, "EVENT_LONG_PRESSED", LV_EVENT_LONG_PRESSED);
  set_int_field(L, "EVENT_LONG_PRESSED_REPEAT", LV_EVENT_LONG_PRESSED_REPEAT);
  set_int_field(L, "EVENT_VALUE_CHANGED", LV_EVENT_VALUE_CHANGED);
  set_int_field(L, "EVENT_GESTURE", LV_EVENT_GESTURE);
  set_int_field(L, "EVENT_KEY", LV_EVENT_KEY);
  set_int_field(L, "EVENT_FOCUSED", LV_EVENT_FOCUSED);
  set_int_field(L, "EVENT_DEFOCUSED", LV_EVENT_DEFOCUSED);
  set_int_field(L, "EVENT_DELETE", LV_EVENT_DELETE);
  set_int_field(L, "EVENT_SCREEN_UNLOAD_START", LV_EVENT_SCREEN_UNLOAD_START);
  set_int_field(L, "EVENT_SCREEN_LOAD_START", LV_EVENT_SCREEN_LOAD_START);
  set_int_field(L, "EVENT_SCREEN_LOADED", LV_EVENT_SCREEN_LOADED);
  set_int_field(L, "EVENT_SCREEN_UNLOADED", LV_EVENT_SCREEN_UNLOADED);

  lua_setglobal(L, "lvgl");

  lua_newtable(L);  // ui
  lua_pushcfunction(L, l_ui_hide_topbar);
  lua_setfield(L, -2, "hide_topbar");
  lua_pushcfunction(L, l_ui_show_topbar);
  lua_setfield(L, -2, "show_topbar");
  lua_pushcfunction(L, l_ui_show_notification);
  lua_setfield(L, -2, "show_notification");
  lua_pushcfunction(L, l_ui_close_notification);
  lua_setfield(L, -2, "close_notification");
  lua_setglobal(L, "ui");
}

void cleanup_lvgl_api(lua_State *L) {
  LuaLvglContext *ctx = get_lvgl_ctx(L);
  if (ctx == nullptr) return;
  ctx->alive = false;

  for (auto &entry : ctx->timers) {
    LuaTimerCb *cb = entry.second;
    if (cb == nullptr) continue;
    lv_timer_t *timer = cb->timer;
    retire_lua_timer_cb(L, ctx, cb);
    if (timer != nullptr) {
      lvgl_call_void([timer]() {
        timer->user_data = nullptr;
        lv_timer_del(timer);
      });
    }
  }
  ctx->timers.clear();

  std::vector<LuaEventCb *> event_cbs(ctx->event_cbs.begin(), ctx->event_cbs.end());
  for (LuaEventCb *cb : event_cbs) {
    if (cb == nullptr) continue;
    lv_obj_t *obj = cb->obj;
    struct _lv_event_dsc_t *dsc = cb->dsc;
    bool obj_valid = false;
    if (obj != nullptr && dsc != nullptr) {
      obj_valid = lvgl_call_ret([obj]() -> bool { return lv_obj_is_valid(obj); });
    }
    if (obj_valid) {
      lvgl_call_void([obj, dsc]() { lv_obj_remove_event_dsc(obj, dsc); });
    }
    delete_lua_event_cb(L, cb);
  }

  for (auto &entry : ctx->fonts) {
    lv_font_t *font = entry.first;
    LuaFontKind kind = entry.second;
    if (font == nullptr) continue;
    lvgl_call_void([=]() {
      switch (kind) {
        case LuaFontKind::BIN:
          lv_font_free(font);
          break;
        case LuaFontKind::TINY_TTF:
#if LV_USE_TINY_TTF
          lv_tiny_ttf_destroy(font);
#endif
          break;
      }
    });
  }
  ctx->fonts.clear();
  if (ctx->queue) {
    vQueueDelete(ctx->queue);
    ctx->queue = nullptr;
  }
  sweep_retired_lua_timer_cbs(ctx);
  set_lvgl_ctx(L, nullptr);
  delete ctx;
}

}  // namespace lua_runtime
}  // namespace esphome
#endif
