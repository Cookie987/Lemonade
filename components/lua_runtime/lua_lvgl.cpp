#include "lua_lvgl.h"

#include <unordered_map>
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
void register_lvgl_api(lua_State *, const std::string &) {}
void cleanup_lvgl_api(lua_State *) {}
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

struct LuaTimerCb;

enum class LuaFontKind : uint8_t {
  TINY_TTF = 0,
};

struct LuaLvglContext {
  QueueHandle_t queue{nullptr};
  volatile bool alive{true};
  std::unordered_map<lv_timer_t *, LuaTimerCb *> timers;
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

static lv_obj_t *get_app_page(const std::string &app_dir) {
  ensure_page_lock();
  if (g_page_lock) xSemaphoreTake(g_page_lock, portMAX_DELAY);
  lv_obj_t *page = nullptr;
  auto it = g_app_pages.find(app_dir);
  if (it != g_app_pages.end()) page = it->second;
  if (g_page_lock) xSemaphoreGive(g_page_lock);
  return page;
}

static std::string dir_from_path(const std::string &path) {
  size_t pos = path.find_last_of('/');
  if (pos == std::string::npos) return ".";
  if (pos == 0) return "/";
  return path.substr(0, pos);
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
    }
    if (++processed >= 256) break;
  }
}

static void wait_job_done(LvglJobBase *job) {
  while (job != nullptr && !job->done) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
  }
}

template<typename F> static void lvgl_call_void(F &&fn) {
  ensure_lvgl_job_queue();
  void *self = (void *) xTaskGetCurrentTaskHandle();
  if (g_lvgl_owner_task == nullptr || self == g_lvgl_owner_task || g_lvgl_job_queue == nullptr) {
    fn();
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

#define LUA_LVGL_IMPL
#include "lua_lvgl_gen.h"
#undef LUA_LVGL_IMPL

struct LuaEventCb {
  LuaLvglContext *ctx;
  int ref;
  int code_filter;
};

struct LuaTimerCb {
  LuaLvglContext *ctx;
  int ref;
  int user_data_ref;
  lv_timer_t *timer;
  bool active;
};

enum class LuaLvglMsgKind : int {
  EVENT = 0,
  TIMER = 1,
};

struct LuaLvglMsg {
  LuaLvglMsgKind kind;
  void *cb;
  void *target;
  int code;
};

static void lua_event_trampoline(lv_event_t *e) {
  auto *cb = static_cast<LuaEventCb *>(lv_event_get_user_data(e));
  if (cb == nullptr || cb->ctx == nullptr || !cb->ctx->alive || cb->ctx->queue == nullptr) return;
  if (cb->ref == LUA_NOREF) return;

  lv_event_code_t code = lv_event_get_code(e);
  if (code != LV_EVENT_DELETE && cb->code_filter != LV_EVENT_ALL && code != cb->code_filter) return;

  LuaLvglMsg msg{LuaLvglMsgKind::EVENT, cb, lv_event_get_target(e), (int) code};
  xQueueSend(cb->ctx->queue, &msg, 0);
}

static void lua_timer_trampoline(lv_timer_t *timer) {
  auto *cb = static_cast<LuaTimerCb *>(timer != nullptr ? timer->user_data : nullptr);
  if (cb == nullptr || cb->ctx == nullptr || !cb->ctx->alive || cb->ctx->queue == nullptr) return;
  if (!cb->active || cb->ref == LUA_NOREF) return;

  LuaLvglMsg msg{LuaLvglMsgKind::TIMER, cb, timer, 0};
  xQueueSend(cb->ctx->queue, &msg, 0);
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
            if (cb->ref != LUA_NOREF) {
              luaL_unref(L, LUA_REGISTRYINDEX, cb->ref);
              cb->ref = LUA_NOREF;
            }
          } else if (cb->ref != LUA_NOREF) {
            lua_rawgeti(L, LUA_REGISTRYINDEX, cb->ref);
            lua_pushlightuserdata(L, obj);
            lua_pushinteger(L, msg.code);
            if (lua_pcall(L, 2, 0, 0) != LUA_OK) {
              const char *err = lua_tostring(L, -1);
              ESP_LOGE(TAG, "event cb error: %s", err ? err : "(unknown)");
              lua_pop(L, 1);
            }
          }
          processed++;
        }
      } else if (msg.kind == LuaLvglMsgKind::TIMER) {
        auto *cb = static_cast<LuaTimerCb *>(msg.cb);
        if (cb != nullptr && cb->ref != LUA_NOREF) {
          lua_rawgeti(L, LUA_REGISTRYINDEX, cb->ref);
          lua_pushlightuserdata(L, msg.target);
          if (cb->user_data_ref != LUA_NOREF) {
            lua_rawgeti(L, LUA_REGISTRYINDEX, cb->user_data_ref);
          } else {
            lua_pushnil(L);
          }
          if (lua_pcall(L, 2, 0, 0) != LUA_OK) {
            const char *err = lua_tostring(L, -1);
            ESP_LOGE(TAG, "timer cb error: %s", err ? err : "(unknown)");
            lua_pop(L, 1);
          }
          processed++;
        }
      }
    } while (xQueueReceive(ctx->queue, &msg, 0) == pdTRUE);
  }

  lua_pushinteger(L, processed);
  return 1;
}
static int l_obj_add_event_cb(lua_State *L) {
  lv_obj_t *obj = check_obj(L, 1);
  int code = (int) luaL_checkinteger(L, 2);
  luaL_checktype(L, 3, LUA_TFUNCTION);
  if (obj == nullptr) return 0;

  LuaLvglContext *ctx = get_lvgl_ctx(L);
  if (ctx == nullptr || !ctx->alive) return 0;

  lua_pushvalue(L, 3);
  int ref = luaL_ref(L, LUA_REGISTRYINDEX);
  auto *cb = new LuaEventCb{ctx, ref, code};
  lvgl_call_void([&]() { lv_obj_add_event_cb(obj, lua_event_trampoline, LV_EVENT_ALL, cb); });
  return 0;
}

static int l_btn_set_text(lua_State *L) {
  lv_obj_t *btn = check_obj(L, 1);
  const char *text = luaL_checkstring(L, 2);
  if (btn == nullptr) return 0;
  lvgl_call_void([&]() {
    lv_obj_t *label = lv_obj_get_child(btn, 0);
    if (label == nullptr) {
      label = lv_label_create(btn);
    }
    lv_label_set_text(label, text);
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
    lvgl_call_void([&]() { lv_label_set_text(obj, fallback); });
    return 0;
  }

  lua_pushvalue(L, 2);
  for (int i = 3; i <= top; i++) lua_pushvalue(L, i);

  if (lua_pcall(L, top - 1, 1, 0) != LUA_OK) {
    const char *err = lua_tostring(L, -1);
    ESP_LOGE(TAG, "label_set_text_fmt error: %s", err ? err : "(unknown)");
    lua_pop(L, 2);
    const char *fallback = luaL_checkstring(L, 2);
    lvgl_call_void([&]() { lv_label_set_text(obj, fallback); });
    return 0;
  }

  const char *text = lua_tostring(L, -1);
  if (text != nullptr) {
    lvgl_call_void([&]() { lv_label_set_text(obj, text); });
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
  std::vector<char> buf((size_t) size, '\0');
  lvgl_call_void([&]() { lv_dropdown_get_selected_str(obj, buf.data(), (uint32_t) buf.size()); });
  lua_pushstring(L, buf.data());
#else
  lua_pushnil(L);
#endif
  return 1;
}

static int l_obj_center(lua_State *L) {
  lv_obj_t *obj = check_obj(L, 1);
  if (obj == nullptr) return 0;
  lvgl_call_void([&]() { lv_obj_center(obj); });
  return 0;
}

static int l_font_load(lua_State *L) {
  const char *path = luaL_checkstring(L, 1);
  lv_coord_t font_size = (lv_coord_t) luaL_checkinteger(L, 2);
  size_t cache_size = (size_t) luaL_optinteger(L, 3, 4096);

  LuaLvglContext *ctx = get_lvgl_ctx(L);
  if (ctx == nullptr || !ctx->alive) {
    lua_pushnil(L);
    return 1;
  }

#if LV_USE_TINY_TTF && LV_TINY_TTF_FILE_SUPPORT
  lv_font_t *font = lvgl_call_ret([&]() -> lv_font_t * {
    return lv_tiny_ttf_create_file_ex(path, font_size, cache_size);
  });
  if (font == nullptr) {
    lua_pushnil(L);
    return 1;
  }
  ctx->fonts[font] = LuaFontKind::TINY_TTF;
  lua_pushlightuserdata(L, (void *) font);
#else
  (void) path;
  (void) font_size;
  (void) cache_size;
  lua_pushnil(L);
#endif
  return 1;
}

static int l_font_free(lua_State *L) {
  lv_font_t *font = (lv_font_t *) lua_touserdata(L, 1);
  if (font == nullptr) return 0;

  LuaLvglContext *ctx = get_lvgl_ctx(L);
  LuaFontKind kind = LuaFontKind::TINY_TTF;
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

  lvgl_call_void([&]() {
    switch (kind) {
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

  auto *cb = new LuaTimerCb{ctx, LUA_NOREF, LUA_NOREF, nullptr, true};
  lua_pushvalue(L, 1);
  cb->ref = luaL_ref(L, LUA_REGISTRYINDEX);
  if (!lua_isnoneornil(L, 3)) {
    lua_pushvalue(L, 3);
    cb->user_data_ref = luaL_ref(L, LUA_REGISTRYINDEX);
  }

  lv_timer_t *timer = lvgl_call_ret([&]() -> lv_timer_t * {
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
        cb->active = false;
        cb->timer = nullptr;
        if (cb->ref != LUA_NOREF) {
          luaL_unref(L, LUA_REGISTRYINDEX, cb->ref);
          cb->ref = LUA_NOREF;
        }
        if (cb->user_data_ref != LUA_NOREF) {
          luaL_unref(L, LUA_REGISTRYINDEX, cb->user_data_ref);
          cb->user_data_ref = LUA_NOREF;
        }
      }
    }
  }

  lvgl_call_void([&]() { lv_timer_del(timer); });
  return 0;
}

static void set_int_field(lua_State *L, const char *name, int value) {
  lua_pushinteger(L, value);
  lua_setfield(L, -2, name);
}

void register_lvgl_api(lua_State *L, const std::string &script_path) {
  std::string app_dir = dir_from_path(script_path);
  lv_obj_t *page = get_app_page(app_dir);
  set_app_page(L, page);

  auto *ctx = new LuaLvglContext;
  ctx->queue = xQueueCreate(16, sizeof(LuaLvglMsg));
  ctx->alive = true;
  set_lvgl_ctx(L, ctx);

  lua_newtable(L);  // lvgl
  lua_pushcfunction(L, l_app_page);
  lua_setfield(L, -2, "app_page");
  lua_pushcfunction(L, l_poll_events);
  lua_setfield(L, -2, "poll_events");

  register_lvgl_gen(L);

  lua_pushcfunction(L, l_obj_add_event_cb);
  lua_setfield(L, -2, "obj_add_event_cb");
  lua_pushcfunction(L, l_btn_set_text);
  lua_setfield(L, -2, "btn_set_text");
  lua_pushcfunction(L, l_label_set_text_fmt);
  lua_setfield(L, -2, "label_set_text_fmt");
  lua_pushcfunction(L, l_dropdown_get_selected_str);
  lua_setfield(L, -2, "dropdown_get_selected_str");
  lua_pushcfunction(L, l_obj_center);
  lua_setfield(L, -2, "obj_center");
  lua_pushcfunction(L, l_font_load);
  lua_setfield(L, -2, "font_load");
  lua_pushcfunction(L, l_font_free);
  lua_setfield(L, -2, "font_free");
  lua_pushcfunction(L, l_timer_create);
  lua_setfield(L, -2, "timer_create");
  lua_pushcfunction(L, l_timer_del);
  lua_setfield(L, -2, "timer_del");
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
  set_int_field(L, "FLAG_SCROLLABLE", LV_OBJ_FLAG_SCROLLABLE);

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

  // opacity
  set_int_field(L, "OPA_TRANSP", LV_OPA_TRANSP);
  set_int_field(L, "OPA_COVER", LV_OPA_COVER);

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
  set_int_field(L, "EVENT_FOCUSED", LV_EVENT_FOCUSED);
  set_int_field(L, "EVENT_DEFOCUSED", LV_EVENT_DEFOCUSED);
  set_int_field(L, "EVENT_DELETE", LV_EVENT_DELETE);

  lua_setglobal(L, "lvgl");
}

void cleanup_lvgl_api(lua_State *L) {
  LuaLvglContext *ctx = get_lvgl_ctx(L);
  if (ctx == nullptr) return;
  ctx->alive = false;
  for (auto &entry : ctx->timers) {
    LuaTimerCb *cb = entry.second;
    if (cb == nullptr) continue;
    cb->active = false;
    cb->ctx = nullptr;
    cb->timer = nullptr;
    if (cb->ref != LUA_NOREF) {
      luaL_unref(L, LUA_REGISTRYINDEX, cb->ref);
      cb->ref = LUA_NOREF;
    }
    if (cb->user_data_ref != LUA_NOREF) {
      luaL_unref(L, LUA_REGISTRYINDEX, cb->user_data_ref);
      cb->user_data_ref = LUA_NOREF;
    }
  }
  ctx->timers.clear();
  for (auto &entry : ctx->fonts) {
    lv_font_t *font = entry.first;
    LuaFontKind kind = entry.second;
    if (font == nullptr) continue;
    lvgl_call_void([&]() {
      switch (kind) {
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
  // Keep ctx allocated to avoid UAF from late LVGL delete events carrying user_data.
  set_lvgl_ctx(L, nullptr);
}

}  // namespace lua_runtime
}  // namespace esphome
#endif
