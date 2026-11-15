#pragma once

#include <string>

struct lua_State;
struct _lv_obj_t;
typedef struct _lv_obj_t lv_obj_t;

namespace esphome {
namespace lua_runtime {

void register_app_page(const std::string &app_dir, lv_obj_t *page);
void unregister_app_page(const std::string &app_dir);
bool is_active_app_page();
void register_lvgl_api(lua_State *L, const std::string &script_path);
void cleanup_lvgl_api(lua_State *L);
void show_lua_error_on_app_page(const std::string &script_path, const std::string &message);

// Deferred app page launch: on icon click the page is not shown immediately, it is shown
// once the Lua UI reports ready (ui.ready() or the first poll_events) so the renderer never
// sees a half-built page, or after a timeout so a broken script cannot leave the user on
// the home screen. While waiting, a cover animation is shown over the icon and it swallows
// input until the page appears or the launch is cancelled.
bool is_app_page_ready(const std::string &app_dir);
void request_app_page_launch(const std::string &app_dir, lv_obj_t *icon);
// Returns true when a pending launch was actually cancelled (the caller can then restore
// the launched-from page's chrome, e.g. hide the status bar again).
bool cancel_app_page_launch();
void dismiss_launch_cover();
void notify_app_page_ready(const std::string &app_dir);
void tick_app_page_launch();

// App lifecycle helpers: which app page is on screen, and dropping a stopped app's widgets.
std::string active_app_dir();
void release_app_page(const std::string &app_dir);
// True while an app is being launched / has never been foreground yet. The runtime must not
// stop or park it: its page is still being built and is not the active screen yet.
bool is_app_launching(const std::string &app_dir);
// True while a tapped app is waiting to be shown.
bool is_app_launch_pending();
// True while any app is still in its launch window (requested/rebuilt but not foreground
// yet). The app-center refresh uses this to avoid abort_all racing a launch.
bool is_any_app_launching();
// Re-arms a launch request without the cover animation (used when a previous task was
// aborted and the app is restarted by the runtime).
void rearm_app_page_launch(const std::string &app_dir);

// Dispatch LVGL API calls onto the LVGL owner task (main loop task).
void set_lvgl_owner_task(void *task_handle);
void process_lvgl_jobs();

}  // namespace lua_runtime
}  // namespace esphome
