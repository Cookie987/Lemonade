#pragma once

#include <map>
#include <string>
#include <vector>

#include "esphome/components/http_request/http_request.h"
#include "esphome/core/component.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

namespace esphome {
namespace lua_runtime {
class LuaRuntime;
}
namespace app_store {

class AppStore : public Component, public Parented<http_request::HttpRequestComponent> {
 public:
  struct RemoteApp {
    std::string id;
    std::string name;
    std::string version;
    std::string description;
    std::string author;
    std::string icon_url;
    std::string package_url;
    std::string package_sha256;
  };

  void setup() override;
  void dump_config() override;

  void set_index_url(const std::string &index_url) { this->index_url_ = index_url; }
  void set_platform(const std::string &platform) { this->platform_ = platform; }
  void set_lua_runtime(lua_runtime::LuaRuntime *lua_runtime) { this->lua_runtime_ = lua_runtime; }

  bool request_refresh();
  bool request_install_selected();
  bool request_uninstall_selected();
  void request_refresh_if_empty();

  void set_selected_index(int index);
  int get_selected_index() const;

  size_t get_entry_count() const;
  bool has_entry(size_t index) const;
  bool has_more_entries(size_t offset, size_t page_size) const;

  std::string get_entry_name(size_t index) const;
  std::string get_entry_meta(size_t index) const;
  std::string get_selected_name() const;
  std::string get_selected_detail_text() const;
  std::string get_selected_primary_action_label() const;
  bool selected_can_uninstall() const;

  bool is_busy() const;
  std::string get_status_message() const;
  bool consume_ui_refresh_requested();
  bool consume_external_apps_refresh_requested();
  bool consume_notification(std::string &message, int &delay_ms);

 protected:
  enum class WorkerOp {
    NONE,
    REFRESH,
    INSTALL,
    UNINSTALL,
  };

  bool start_worker_(WorkerOp op, const std::string &app_id);
  static void worker_task_entry_(void *param);
  void run_worker_(WorkerOp op, std::string app_id);

  bool fetch_index_();
  bool install_app_(const std::string &app_id);
  bool uninstall_app_(const std::string &app_id);
  bool read_http_text_(const std::string &url, std::string &body, std::string &error);
  bool download_to_file_(const std::string &url, const std::string &dst_path, std::string &error);
  bool extract_zip_to_dir_(const std::string &zip_path, const std::string &dst_dir, std::string &error);
  bool validate_stage_manifest_(const std::string &stage_dir, const RemoteApp &entry, std::string &error) const;
  bool read_file_text_(const std::string &path, std::string &body, std::string &error) const;
  bool write_file_text_(const std::string &path, const std::string &body, std::string &error) const;

  void ensure_store_dirs_();
  void scan_installed_apps_();
  bool parse_index_document_(const std::string &body, std::vector<RemoteApp> &entries, std::string &error) const;
  RemoteApp find_entry_copy_(const std::string &app_id, bool *found = nullptr) const;
  std::string installed_version_for_(const std::string &app_id) const;
  bool is_installed_(const std::string &app_id) const;
  bool is_upgrade_available_(const std::string &app_id, const std::string &remote_version) const;

  static std::string resolve_url_(const std::string &base_url, const std::string &value);
  static std::string normalize_relative_path_(const std::string &path);
  static bool mkdir_p_(const std::string &path);
  static bool ensure_parent_dir_(const std::string &path);
  static bool remove_recursive_(const std::string &path);
  static bool path_exists_(const std::string &path);
  static bool is_directory_(const std::string &path);

  void set_status_(const std::string &message, bool ui_refresh = true);
  void queue_notification_(const std::string &message, int delay_ms = 2500);
  void mark_external_apps_refresh_();

  mutable SemaphoreHandle_t mutex_{nullptr};
  std::string index_url_;
  std::string platform_{"s3_t"};
  lua_runtime::LuaRuntime *lua_runtime_{nullptr};

  std::vector<RemoteApp> entries_;
  std::map<std::string, std::string> installed_versions_;
  std::map<std::string, std::string> installed_descriptions_;
  int selected_index_{-1};

  bool busy_{false};
  bool worker_running_{false};
  bool ui_refresh_requested_{true};
  bool external_apps_refresh_requested_{false};
  std::string status_message_{"应用商店未加载"};
  bool notification_requested_{false};
  std::string notification_message_;
  int notification_delay_ms_{2500};
};

}  // namespace app_store
}  // namespace esphome
