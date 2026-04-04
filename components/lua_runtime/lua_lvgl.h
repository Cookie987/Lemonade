#pragma once

#include <string>

struct lua_State;
struct _lv_obj_t;
typedef struct _lv_obj_t lv_obj_t;

namespace esphome {
namespace lua_runtime {

void register_app_page(const std::string &app_dir, lv_obj_t *page);
void register_lvgl_api(lua_State *L, const std::string &script_path);
void cleanup_lvgl_api(lua_State *L);

// Dispatch LVGL API calls onto the LVGL owner task (main loop task).
void set_lvgl_owner_task(void *task_handle);
void process_lvgl_jobs();

}  // namespace lua_runtime
}  // namespace esphome
