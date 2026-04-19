#include "app_store.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include <zlib.h>

#include "esp_heap_caps.h"
#include "esphome/components/json/json_util.h"
#include "esphome/components/lua_runtime/lua_runtime.h"
#include "esphome/core/application.h"
#include "esphome/core/log.h"

namespace esphome {
namespace app_store {

namespace {

static const char *const TAG = "app_store";
static const char *const APP_ROOT = "/sdcard/opt";
static const char *const STORE_ROOT = "/sdcard/var/appstore";
static const char *const STORE_INDEX_PATH = "/sdcard/var/appstore/index.json";
static const char *const TMP_ROOT = "/sdcard/tmp/appstore";
static const char *const TMP_PACKAGE_ROOT = "/sdcard/tmp/appstore/packages";
static const char *const TMP_STAGE_ROOT = "/sdcard/tmp/appstore/stage";
static const char *const BACKUP_ROOT = "/sdcard/var/appstore/backup";
static constexpr configSTACK_DEPTH_TYPE WORKER_STACK_SIZE = 14336;

static constexpr uint32_t ZIP_EOCD_SIGNATURE = 0x06054b50UL;
static constexpr uint32_t ZIP_CENTRAL_SIGNATURE = 0x02014b50UL;
static constexpr uint32_t ZIP_LOCAL_SIGNATURE = 0x04034b50UL;
static constexpr size_t ZIP_EOCD_MIN_SIZE = 22;
static constexpr size_t ZIP_EOCD_MAX_SEARCH = 65535 + ZIP_EOCD_MIN_SIZE;

class LockGuard {
 public:
  explicit LockGuard(SemaphoreHandle_t mutex) : mutex_(mutex) {
    if (this->mutex_ != nullptr) {
      xSemaphoreTake(this->mutex_, portMAX_DELAY);
    }
  }

  ~LockGuard() {
    if (this->mutex_ != nullptr) {
      xSemaphoreGive(this->mutex_);
    }
  }

 protected:
  SemaphoreHandle_t mutex_;
};

struct ZipEntry {
  std::string raw_name;
  std::string normalized_name;
  uint16_t flags{0};
  uint16_t method{0};
  uint32_t crc32{0};
  uint32_t compressed_size{0};
  uint32_t uncompressed_size{0};
  uint32_t local_header_offset{0};
  bool is_directory{false};
};

std::string dirname_of(const std::string &path) {
  size_t pos = path.rfind('/');
  if (pos == std::string::npos) {
    return ".";
  }
  if (pos == 0) {
    return "/";
  }
  return path.substr(0, pos);
}

bool read_exact(FILE *file, void *buffer, size_t size) {
  return size == 0 || fread(buffer, 1, size, file) == size;
}

uint16_t read_le16(const uint8_t *data) {
  return static_cast<uint16_t>(data[0]) | (static_cast<uint16_t>(data[1]) << 8);
}

uint32_t read_le32(const uint8_t *data) {
  return static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8) |
         (static_cast<uint32_t>(data[2]) << 16) | (static_cast<uint32_t>(data[3]) << 24);
}

bool starts_with(const std::string &value, const std::string &prefix) {
  return value.size() >= prefix.size() && value.compare(0, prefix.size(), prefix) == 0;
}

std::string strip_common_root(const std::string &path, const std::string &common_root) {
  if (common_root.empty()) {
    return path;
  }
  if (path == common_root) {
    return "";
  }
  std::string prefix = common_root + "/";
  if (starts_with(path, prefix)) {
    return path.substr(prefix.size());
  }
  return path;
}

std::string detect_common_root(const std::vector<ZipEntry> &entries) {
  std::string common_root;
  for (const auto &entry : entries) {
    if (entry.normalized_name.empty() || entry.is_directory) {
      continue;
    }
    size_t slash = entry.normalized_name.find('/');
    if (slash == std::string::npos) {
      return "";
    }
    std::string root = entry.normalized_name.substr(0, slash);
    if (common_root.empty()) {
      common_root = root;
      continue;
    }
    if (common_root != root) {
      return "";
    }
  }
  return common_root;
}

}  // namespace

void AppStore::setup() {
  this->mutex_ = xSemaphoreCreateMutex();
  this->ensure_store_dirs_();
  this->scan_installed_apps_();
  ESP_LOGI(TAG, "App store initialized, index=%s platform=%s", this->index_url_.c_str(), this->platform_.c_str());
  this->set_status_("应用商店准备就绪");
}

void AppStore::dump_config() {
  ESP_LOGCONFIG(TAG, "App Store");
  ESP_LOGCONFIG(TAG, "  Index URL: %s", this->index_url_.c_str());
  ESP_LOGCONFIG(TAG, "  Platform: %s", this->platform_.c_str());
  ESP_LOGCONFIG(TAG, "  App Root: %s", APP_ROOT);
  ESP_LOGCONFIG(TAG, "  Temp Root: %s", TMP_ROOT);
  ESP_LOGCONFIG(TAG, "  Data Root: %s", STORE_ROOT);
}

bool AppStore::request_refresh() { return this->start_worker_(WorkerOp::REFRESH, ""); }

void AppStore::request_refresh_if_empty() {
  if (this->get_entry_count() == 0 && !this->is_busy()) {
    this->request_refresh();
  }
}

bool AppStore::request_install_selected() {
  std::string app_id;
  {
    LockGuard lock(this->mutex_);
    if (this->selected_index_ < 0 || static_cast<size_t>(this->selected_index_) >= this->entries_.size()) {
      ESP_LOGW(TAG, "Install requested without a selected app");
      this->status_message_ = "未选择应用";
      this->ui_refresh_requested_ = true;
      this->notification_requested_ = true;
      this->notification_message_ = "请先选择要安装的应用";
      this->notification_delay_ms_ = 2200;
      return false;
    }
    app_id = this->entries_[this->selected_index_].id;
  }
  return this->start_worker_(WorkerOp::INSTALL, app_id);
}

bool AppStore::request_uninstall_selected() {
  std::string app_id;
  {
    LockGuard lock(this->mutex_);
    if (this->selected_index_ < 0 || static_cast<size_t>(this->selected_index_) >= this->entries_.size()) {
      ESP_LOGW(TAG, "Uninstall requested without a selected app");
      this->status_message_ = "未选择应用";
      this->ui_refresh_requested_ = true;
      this->notification_requested_ = true;
      this->notification_message_ = "请先选择要卸载的应用";
      this->notification_delay_ms_ = 2200;
      return false;
    }
    app_id = this->entries_[this->selected_index_].id;
  }
  return this->start_worker_(WorkerOp::UNINSTALL, app_id);
}

void AppStore::set_selected_index(int index) {
  LockGuard lock(this->mutex_);
  if (index < 0 || static_cast<size_t>(index) >= this->entries_.size()) {
    this->selected_index_ = -1;
  } else {
    this->selected_index_ = index;
  }
  this->ui_refresh_requested_ = true;
}

int AppStore::get_selected_index() const {
  LockGuard lock(this->mutex_);
  return this->selected_index_;
}

size_t AppStore::get_entry_count() const {
  LockGuard lock(this->mutex_);
  return this->entries_.size();
}

bool AppStore::has_entry(size_t index) const {
  LockGuard lock(this->mutex_);
  return index < this->entries_.size();
}

bool AppStore::has_more_entries(size_t offset, size_t page_size) const {
  LockGuard lock(this->mutex_);
  return offset + page_size < this->entries_.size();
}

std::string AppStore::get_entry_name(size_t index) const {
  LockGuard lock(this->mutex_);
  if (index >= this->entries_.size()) {
    return "";
  }
  return this->entries_[index].name.empty() ? this->entries_[index].id : this->entries_[index].name;
}

std::string AppStore::get_entry_meta(size_t index) const {
  LockGuard lock(this->mutex_);
  if (index >= this->entries_.size()) {
    return "";
  }
  const auto &entry = this->entries_[index];
  std::string state = this->is_upgrade_available_(entry.id, entry.version)
                          ? "可升级"
                          : (this->is_installed_(entry.id) ? "已安装" : "未安装");
  return "版本 " + entry.version + " | " + state;
}

std::string AppStore::get_selected_name() const {
  LockGuard lock(this->mutex_);
  if (this->selected_index_ < 0 || static_cast<size_t>(this->selected_index_) >= this->entries_.size()) {
    return "应用详情";
  }
  const auto &entry = this->entries_[this->selected_index_];
  return entry.name.empty() ? entry.id : entry.name;
}

std::string AppStore::get_selected_detail_text() const {
  LockGuard lock(this->mutex_);
  if (this->selected_index_ < 0 || static_cast<size_t>(this->selected_index_) >= this->entries_.size()) {
    return "请选择一个应用。";
  }

  const auto &entry = this->entries_[this->selected_index_];
  std::string detail = "ID: " + entry.id;
  detail += "\n版本: " + entry.version;
  detail += "\n状态: ";
  detail += (this->is_upgrade_available_(entry.id, entry.version) ? "可升级"
                                                                  : (this->is_installed_(entry.id) ? "已安装" : "未安装"));

  std::string installed_version = this->installed_version_for_(entry.id);
  if (!installed_version.empty()) {
    detail += "\n本地版本: " + installed_version;
  }
  if (!entry.author.empty()) {
    detail += "\n作者: " + entry.author;
  }
  std::string description = entry.description;
  if (description.empty()) {
    auto it = this->installed_descriptions_.find(entry.id);
    if (it != this->installed_descriptions_.end()) {
      description = it->second;
    }
  }
  if (!description.empty()) {
    detail += "\n\n" + description;
  }
  return detail;
}

std::string AppStore::get_selected_primary_action_label() const {
  LockGuard lock(this->mutex_);
  if (this->selected_index_ < 0 || static_cast<size_t>(this->selected_index_) >= this->entries_.size()) {
    return "安装";
  }
  const auto &entry = this->entries_[this->selected_index_];
  if (this->is_upgrade_available_(entry.id, entry.version)) {
    return "升级";
  }
  if (this->is_installed_(entry.id)) {
    return "重装";
  }
  return "安装";
}

bool AppStore::selected_can_uninstall() const {
  LockGuard lock(this->mutex_);
  if (this->selected_index_ < 0 || static_cast<size_t>(this->selected_index_) >= this->entries_.size()) {
    return false;
  }
  return this->is_installed_(this->entries_[this->selected_index_].id);
}

bool AppStore::is_busy() const {
  LockGuard lock(this->mutex_);
  return this->busy_;
}

std::string AppStore::get_status_message() const {
  LockGuard lock(this->mutex_);
  return this->status_message_;
}

bool AppStore::consume_notification(std::string &message, int &delay_ms) {
  LockGuard lock(this->mutex_);
  if (!this->notification_requested_) {
    return false;
  }
  message = this->notification_message_;
  delay_ms = this->notification_delay_ms_;
  this->notification_requested_ = false;
  this->notification_message_.clear();
  return true;
}

bool AppStore::consume_ui_refresh_requested() {
  LockGuard lock(this->mutex_);
  bool requested = this->ui_refresh_requested_;
  this->ui_refresh_requested_ = false;
  return requested;
}

bool AppStore::consume_external_apps_refresh_requested() {
  LockGuard lock(this->mutex_);
  bool requested = this->external_apps_refresh_requested_;
  this->external_apps_refresh_requested_ = false;
  return requested;
}

bool AppStore::start_worker_(WorkerOp op, const std::string &app_id) {
  {
    LockGuard lock(this->mutex_);
    if (this->worker_running_) {
      ESP_LOGW(TAG, "Reject worker start because another task is already running");
      this->status_message_ = "已有任务在进行中";
      this->ui_refresh_requested_ = true;
      this->notification_requested_ = true;
      this->notification_message_ = "应用商店已有任务在进行中";
      this->notification_delay_ms_ = 2000;
      return false;
    }
    this->worker_running_ = true;
    this->busy_ = true;
    if (op == WorkerOp::REFRESH) {
      this->status_message_ = "正在刷新应用列表";
    } else if (op == WorkerOp::INSTALL) {
      this->status_message_ = "正在安装 " + app_id;
    } else if (op == WorkerOp::UNINSTALL) {
      this->status_message_ = "正在卸载 " + app_id;
    }
    this->ui_refresh_requested_ = true;
  }
  ESP_LOGI(TAG, "Starting app store worker op=%d app=%s", static_cast<int>(op), app_id.c_str());

  struct WorkerArgs {
    AppStore *self;
    WorkerOp op;
    std::string app_id;
  };

  auto *args = new WorkerArgs{this, op, app_id};
  size_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  size_t internal_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  size_t psram_largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  ESP_LOGI(TAG,
           "Worker memory before create: internal_free=%u internal_largest=%u psram_free=%u psram_largest=%u stack=%u",
           static_cast<unsigned>(internal_free), static_cast<unsigned>(internal_largest), static_cast<unsigned>(psram_free),
           static_cast<unsigned>(psram_largest), static_cast<unsigned>(WORKER_STACK_SIZE));

  // The app-store worker performs network, filesystem, and zip processing. Keep
  // its large stack in PSRAM first so fragmented internal RAM does not prevent
  // task creation after the device has been running for hours.
  BaseType_t ok = xTaskCreatePinnedToCoreWithCaps(&AppStore::worker_task_entry_, "app_store", WORKER_STACK_SIZE, args, 1,
                                                  nullptr, tskNO_AFFINITY, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (ok != pdPASS) {
    ESP_LOGW(TAG, "Worker create in PSRAM failed, retrying with internal RAM");
    ok = xTaskCreate(&AppStore::worker_task_entry_, "app_store", WORKER_STACK_SIZE, args, 1, nullptr);
  }
  if (ok != pdPASS) {
    delete args;
    LockGuard lock(this->mutex_);
    this->worker_running_ = false;
    this->busy_ = false;
    this->status_message_ = "无法创建应用商店任务";
    this->ui_refresh_requested_ = true;
    this->notification_requested_ = true;
    this->notification_message_ = "无法创建应用商店任务";
    this->notification_delay_ms_ = 2500;
    internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    internal_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    psram_largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_LOGE(TAG,
             "Failed to create worker task internal_free=%u internal_largest=%u psram_free=%u psram_largest=%u stack=%u",
             static_cast<unsigned>(internal_free), static_cast<unsigned>(internal_largest), static_cast<unsigned>(psram_free),
             static_cast<unsigned>(psram_largest), static_cast<unsigned>(WORKER_STACK_SIZE));
    return false;
  }
  return true;
}

void AppStore::worker_task_entry_(void *param) {
  struct WorkerArgs {
    AppStore *self;
    WorkerOp op;
    std::string app_id;
  };

  auto *args = static_cast<WorkerArgs *>(param);
  AppStore *self = args->self;
  WorkerOp op = args->op;
  std::string app_id = args->app_id;
  delete args;

  self->run_worker_(op, app_id);
  vTaskDeleteWithCaps(nullptr);
}

void AppStore::run_worker_(WorkerOp op, std::string app_id) {
  ESP_LOGD(TAG, "Worker running op=%d app=%s", static_cast<int>(op), app_id.c_str());
  bool success = false;
  switch (op) {
    case WorkerOp::REFRESH:
      success = this->fetch_index_();
      break;
    case WorkerOp::INSTALL:
      success = this->install_app_(app_id);
      break;
    case WorkerOp::UNINSTALL:
      success = this->uninstall_app_(app_id);
      break;
    default:
      break;
  }

  LockGuard lock(this->mutex_);
  this->busy_ = false;
  this->worker_running_ = false;
  this->ui_refresh_requested_ = true;
  if (success && this->status_message_.empty()) {
    this->status_message_ = "完成";
  }
  ESP_LOGI(TAG, "Worker finished op=%d app=%s success=%s", static_cast<int>(op), app_id.c_str(), YESNO(success));
}

bool AppStore::fetch_index_() {
  std::string body;
  std::string error;
  bool used_cache = false;

  this->set_status_("正在获取商店索引");
  ESP_LOGI(TAG, "Fetching app index from %s", this->index_url_.c_str());
  if (!this->read_http_text_(this->index_url_, body, error)) {
    ESP_LOGW(TAG, "Fetch index failed, trying local cache: %s", error.c_str());
    std::string cache_error;
    if (!this->read_file_text_(STORE_INDEX_PATH, body, cache_error)) {
      this->set_status_("获取商店索引失败: " + error);
      this->queue_notification_("应用商店刷新失败: " + error, 3500);
      return false;
    }
    used_cache = true;
    this->set_status_("网络失败，正在使用本地缓存");
  }

  std::vector<RemoteApp> entries;
  if (!this->parse_index_document_(body, entries, error)) {
    ESP_LOGE(TAG, "Parse index failed: %s", error.c_str());
    this->set_status_("解析商店索引失败: " + error);
    this->queue_notification_("商店索引解析失败: " + error, 3500);
    return false;
  }
  ESP_LOGI(TAG, "Parsed %u remote app entries", static_cast<unsigned>(entries.size()));

  if (!used_cache) {
    std::string cache_error;
    if (!this->write_file_text_(STORE_INDEX_PATH, body, cache_error)) {
      ESP_LOGW(TAG, "Failed to cache store index: %s", cache_error.c_str());
    }
  }

  this->scan_installed_apps_();
  {
    LockGuard lock(this->mutex_);
    this->entries_ = std::move(entries);
    if (this->selected_index_ >= static_cast<int>(this->entries_.size())) {
      this->selected_index_ = this->entries_.empty() ? -1 : 0;
    }
    if (this->selected_index_ < 0 && !this->entries_.empty()) {
      this->selected_index_ = 0;
    }
    if (this->entries_.empty()) {
      this->status_message_ = "商店为空";
    } else if (used_cache) {
      this->status_message_ = "已加载 " + std::to_string(this->entries_.size()) + " 个应用（缓存）";
    } else {
      this->status_message_ = "已加载 " + std::to_string(this->entries_.size()) + " 个应用";
    }
    this->ui_refresh_requested_ = true;
  }

  if (used_cache) {
    this->queue_notification_("网络失败，已使用本地缓存", 2600);
  } else {
    this->queue_notification_("应用商店已刷新", 1800);
  }
  return true;
}

bool AppStore::install_app_(const std::string &app_id) {
  ESP_LOGI(TAG, "Installing app %s", app_id.c_str());
  bool found = false;
  RemoteApp entry = this->find_entry_copy_(app_id, &found);
  if (!found) {
    ESP_LOGE(TAG, "Install failed, app not found: %s", app_id.c_str());
    this->set_status_("未找到应用: " + app_id);
    this->queue_notification_("未找到应用: " + app_id, 2500);
    return false;
  }
  if (entry.package_url.empty()) {
    ESP_LOGE(TAG, "Install failed, package url is empty: %s", app_id.c_str());
    this->set_status_("应用缺少安装包地址: " + app_id);
    this->queue_notification_("应用缺少安装包地址", 2500);
    return false;
  }

  if (this->lua_runtime_ != nullptr) {
    this->lua_runtime_->abort_all();
  }

  this->ensure_store_dirs_();
  mkdir_p_(APP_ROOT);

  const std::string package_path = std::string(TMP_PACKAGE_ROOT) + "/" + app_id + ".zip";
  const std::string stage_dir = std::string(TMP_STAGE_ROOT) + "/" + app_id;
  const std::string target_dir = std::string(APP_ROOT) + "/" + app_id;
  const std::string backup_dir = std::string(BACKUP_ROOT) + "/" + app_id;

  remove_recursive_(package_path);
  remove_recursive_(stage_dir);
  remove_recursive_(backup_dir);

  std::string error;
  this->set_status_("正在下载安装包");
  ESP_LOGD(TAG, "Downloading package url=%s dst=%s", entry.package_url.c_str(), package_path.c_str());
  if (!this->download_to_file_(entry.package_url, package_path, error)) {
    remove_recursive_(package_path);
    ESP_LOGE(TAG, "Download package failed: %s", error.c_str());
    this->set_status_("下载安装包失败: " + error);
    this->queue_notification_("下载安装包失败: " + error, 3500);
    return false;
  }

  this->set_status_("正在解压安装包");
  if (!this->extract_zip_to_dir_(package_path, stage_dir, error)) {
    remove_recursive_(stage_dir);
    remove_recursive_(package_path);
    ESP_LOGE(TAG, "Extract package failed: %s", error.c_str());
    this->set_status_("解压安装包失败: " + error);
    this->queue_notification_("解压安装包失败: " + error, 3500);
    return false;
  }

  this->set_status_("正在校验应用包");
  if (!this->validate_stage_manifest_(stage_dir, entry, error)) {
    remove_recursive_(stage_dir);
    remove_recursive_(package_path);
    ESP_LOGE(TAG, "Validate package failed: %s", error.c_str());
    this->set_status_("应用包校验失败: " + error);
    this->queue_notification_("应用包校验失败: " + error, 3500);
    return false;
  }

  if (path_exists_(target_dir) && rename(target_dir.c_str(), backup_dir.c_str()) != 0) {
    remove_recursive_(stage_dir);
    remove_recursive_(package_path);
    ESP_LOGE(TAG, "Failed to backup old app dir %s to %s", target_dir.c_str(), backup_dir.c_str());
    this->set_status_("无法备份旧版本");
    this->queue_notification_("无法备份旧版本", 2500);
    return false;
  }

  if (rename(stage_dir.c_str(), target_dir.c_str()) != 0) {
    remove_recursive_(stage_dir);
    remove_recursive_(package_path);
    if (path_exists_(backup_dir)) {
      rename(backup_dir.c_str(), target_dir.c_str());
    }
    ESP_LOGE(TAG, "Failed to move stage dir %s to %s", stage_dir.c_str(), target_dir.c_str());
    this->set_status_("无法写入应用目录");
    this->queue_notification_("无法写入应用目录", 2500);
    return false;
  }

  remove_recursive_(package_path);
  remove_recursive_(backup_dir);

  this->scan_installed_apps_();
  this->mark_external_apps_refresh_();
  ESP_LOGI(TAG, "App installed successfully: %s version=%s", entry.id.c_str(), entry.version.c_str());
  this->set_status_("已安装 " + (entry.name.empty() ? entry.id : entry.name));
  this->queue_notification_("已安装 " + (entry.name.empty() ? entry.id : entry.name), 2200);
  return true;
}

bool AppStore::uninstall_app_(const std::string &app_id) {
  const std::string target_dir = std::string(APP_ROOT) + "/" + app_id;
  if (!path_exists_(target_dir)) {
    ESP_LOGW(TAG, "Uninstall skipped, app not installed: %s", app_id.c_str());
    this->set_status_("应用未安装: " + app_id);
    this->queue_notification_("应用未安装: " + app_id, 2200);
    return false;
  }

  if (this->lua_runtime_ != nullptr) {
    this->lua_runtime_->abort_all();
  }

  this->set_status_("正在删除 " + app_id);
  ESP_LOGI(TAG, "Uninstalling app %s", app_id.c_str());
  if (!remove_recursive_(target_dir)) {
    ESP_LOGE(TAG, "Failed to delete app dir %s", target_dir.c_str());
    this->set_status_("删除应用失败: " + app_id);
    this->queue_notification_("删除应用失败: " + app_id, 3000);
    return false;
  }

  this->scan_installed_apps_();
  this->mark_external_apps_refresh_();
  this->set_status_("已卸载 " + app_id);
  this->queue_notification_("已卸载 " + app_id, 2200);
  return true;
}

bool AppStore::read_http_text_(const std::string &url, std::string &body, std::string &error) {
  ESP_LOGD(TAG, "HTTP GET text %s", url.c_str());
  auto container = this->parent_->get(url);
  if (container == nullptr) {
    error = "请求未发出";
    return false;
  }
  if (!http_request::is_success(container->status_code)) {
    error = "HTTP " + std::to_string(container->status_code);
    container->end();
    return false;
  }

  body.clear();
  if (container->content_length > 0 && container->content_length < (128 * 1024)) {
    body.reserve(container->content_length);
  }

  uint8_t buffer[512];
  uint32_t last_data_time = millis();
  while (true) {
    int read_len = container->read(buffer, sizeof(buffer));
    App.feed_wdt();
    yield();

    auto result = http_request::http_read_loop_result(read_len, last_data_time, this->parent_->get_timeout(),
                                                      container->is_read_complete());
    if (result == http_request::HttpReadLoopResult::DATA) {
      body.append(reinterpret_cast<const char *>(buffer), read_len);
      continue;
    }
    if (result == http_request::HttpReadLoopResult::RETRY) {
      continue;
    }
    if (result == http_request::HttpReadLoopResult::COMPLETE) {
      break;
    }
    error = result == http_request::HttpReadLoopResult::TIMEOUT ? "读取超时" : "读取失败";
    container->end();
    return false;
  }

  container->end();
  ESP_LOGD(TAG, "HTTP text read complete url=%s bytes=%u", url.c_str(), static_cast<unsigned>(body.size()));
  return true;
}

bool AppStore::download_to_file_(const std::string &url, const std::string &dst_path, std::string &error) {
  ESP_LOGD(TAG, "HTTP download url=%s dst=%s", url.c_str(), dst_path.c_str());
  if (!ensure_parent_dir_(dst_path)) {
    error = "无法创建目录";
    return false;
  }

  auto container = this->parent_->get(url);
  if (container == nullptr) {
    error = "请求未发出";
    return false;
  }
  if (!http_request::is_success(container->status_code)) {
    error = "HTTP " + std::to_string(container->status_code);
    container->end();
    return false;
  }

  std::ofstream out(dst_path, std::ios::binary | std::ios::trunc);
  if (!out.is_open()) {
    error = "无法写入文件";
    container->end();
    return false;
  }

  uint8_t buffer[1024];
  uint32_t last_data_time = millis();
  while (true) {
    int read_len = container->read(buffer, sizeof(buffer));
    App.feed_wdt();
    yield();

    auto result = http_request::http_read_loop_result(read_len, last_data_time, this->parent_->get_timeout(),
                                                      container->is_read_complete());
    if (result == http_request::HttpReadLoopResult::DATA) {
      out.write(reinterpret_cast<const char *>(buffer), read_len);
      if (!out.good()) {
        error = "写文件失败";
        out.close();
        container->end();
        return false;
      }
      continue;
    }
    if (result == http_request::HttpReadLoopResult::RETRY) {
      continue;
    }
    if (result == http_request::HttpReadLoopResult::COMPLETE) {
      break;
    }
    error = result == http_request::HttpReadLoopResult::TIMEOUT ? "读取超时" : "下载失败";
    out.close();
    container->end();
    return false;
  }

  out.close();
  container->end();
  ESP_LOGD(TAG, "HTTP download complete url=%s dst=%s", url.c_str(), dst_path.c_str());
  return true;
}

bool AppStore::extract_zip_to_dir_(const std::string &zip_path, const std::string &dst_dir, std::string &error) {
  ESP_LOGI(TAG, "Extracting zip %s -> %s", zip_path.c_str(), dst_dir.c_str());
  if (!mkdir_p_(dst_dir)) {
    error = "无法创建解压目录";
    return false;
  }

  FILE *file = fopen(zip_path.c_str(), "rb");
  if (file == nullptr) {
    error = "无法打开安装包";
    return false;
  }

  if (fseek(file, 0, SEEK_END) != 0) {
    fclose(file);
    error = "无法读取安装包";
    return false;
  }
  long file_size_long = ftell(file);
  if (file_size_long < 0 || static_cast<size_t>(file_size_long) < ZIP_EOCD_MIN_SIZE) {
    fclose(file);
    error = "安装包格式无效";
    return false;
  }
  size_t file_size = static_cast<size_t>(file_size_long);

  size_t search_size = std::min(file_size, ZIP_EOCD_MAX_SEARCH);
  std::vector<uint8_t> tail(search_size);
  if (fseek(file, static_cast<long>(file_size - search_size), SEEK_SET) != 0 ||
      !read_exact(file, tail.data(), tail.size())) {
    fclose(file);
    error = "无法读取安装包目录";
    return false;
  }

  size_t eocd_offset_in_tail = tail.size();
  for (size_t i = tail.size() - ZIP_EOCD_MIN_SIZE;; --i) {
    if (read_le32(&tail[i]) == ZIP_EOCD_SIGNATURE) {
      eocd_offset_in_tail = i;
      break;
    }
    if (i == 0) {
      break;
    }
  }
  if (eocd_offset_in_tail == tail.size()) {
    fclose(file);
    error = "安装包缺少 ZIP 目录";
    return false;
  }

  const uint8_t *eocd = &tail[eocd_offset_in_tail];
  if (read_le16(eocd + 4) != 0 || read_le16(eocd + 6) != 0) {
    fclose(file);
    error = "暂不支持分卷 ZIP";
    return false;
  }

  uint16_t entry_count = read_le16(eocd + 10);
  uint32_t central_dir_size = read_le32(eocd + 12);
  uint32_t central_dir_offset = read_le32(eocd + 16);
  if (entry_count == 0 || central_dir_offset == 0xFFFFFFFFUL || central_dir_size == 0xFFFFFFFFUL) {
    fclose(file);
    error = "暂不支持 ZIP64 或空包";
    return false;
  }
  if (central_dir_offset + central_dir_size > file_size) {
    fclose(file);
    error = "ZIP 目录越界";
    return false;
  }

  if (fseek(file, static_cast<long>(central_dir_offset), SEEK_SET) != 0) {
    fclose(file);
    error = "无法定位 ZIP 目录";
    return false;
  }

  std::vector<ZipEntry> entries;
  entries.reserve(entry_count);
  for (uint16_t i = 0; i < entry_count; i++) {
    uint8_t header[46];
    if (!read_exact(file, header, sizeof(header))) {
      fclose(file);
      error = "ZIP 目录读取失败";
      return false;
    }
    if (read_le32(header) != ZIP_CENTRAL_SIGNATURE) {
      fclose(file);
      error = "ZIP 目录签名无效";
      return false;
    }

    uint16_t flags = read_le16(header + 8);
    uint16_t method = read_le16(header + 10);
    uint32_t crc = read_le32(header + 16);
    uint32_t compressed_size = read_le32(header + 20);
    uint32_t uncompressed_size = read_le32(header + 24);
    uint16_t name_len = read_le16(header + 28);
    uint16_t extra_len = read_le16(header + 30);
    uint16_t comment_len = read_le16(header + 32);
    uint32_t local_header_offset = read_le32(header + 42);

    std::vector<char> name_buffer(name_len);
    if (!read_exact(file, name_buffer.data(), name_buffer.size())) {
      fclose(file);
      error = "ZIP 文件名读取失败";
      return false;
    }

    std::string raw_name(name_buffer.begin(), name_buffer.end());
    bool is_directory = !raw_name.empty() && raw_name.back() == '/';
    std::string normalized_name = this->normalize_relative_path_(raw_name);
    if (normalized_name.empty() && !is_directory) {
      fclose(file);
      error = "ZIP 中包含非法路径";
      return false;
    }

    long skip_len = static_cast<long>(extra_len) + static_cast<long>(comment_len);
    if (skip_len > 0 && fseek(file, skip_len, SEEK_CUR) != 0) {
      fclose(file);
      error = "ZIP 扩展字段读取失败";
      return false;
    }

    entries.push_back(
        {raw_name, normalized_name, flags, method, crc, compressed_size, uncompressed_size, local_header_offset, is_directory});
    ESP_LOGD(TAG, "ZIP entry %u/%u path=%s method=%u comp=%u uncomp=%u dir=%s", static_cast<unsigned>(i + 1),
             static_cast<unsigned>(entry_count), raw_name.c_str(), static_cast<unsigned>(method),
             static_cast<unsigned>(compressed_size), static_cast<unsigned>(uncompressed_size), YESNO(is_directory));
  }

  std::string common_root = detect_common_root(entries);
  if (!common_root.empty()) {
    ESP_LOGI(TAG, "ZIP package has a common root directory: %s", common_root.c_str());
  }

  for (const auto &entry : entries) {
    std::string relative_path = strip_common_root(entry.normalized_name, common_root);
    if (relative_path.empty()) {
      continue;
    }

    const std::string output_path = dst_dir + "/" + relative_path;
    if (entry.is_directory) {
      if (!mkdir_p_(output_path)) {
        fclose(file);
        error = "无法创建目录: " + relative_path;
        return false;
      }
      continue;
    }

    if ((entry.flags & 0x0001U) != 0) {
      fclose(file);
      error = "暂不支持加密 ZIP";
      return false;
    }
    if (!ensure_parent_dir_(output_path)) {
      fclose(file);
      error = "无法创建目录: " + dirname_of(output_path);
      return false;
    }

    if (fseek(file, static_cast<long>(entry.local_header_offset), SEEK_SET) != 0) {
      fclose(file);
      error = "无法定位 ZIP 本地头";
      return false;
    }

    uint8_t local_header[30];
    if (!read_exact(file, local_header, sizeof(local_header))) {
      fclose(file);
      error = "ZIP 本地头读取失败";
      return false;
    }
    if (read_le32(local_header) != ZIP_LOCAL_SIGNATURE) {
      fclose(file);
      error = "ZIP 本地头签名无效";
      return false;
    }

    uint16_t local_name_len = read_le16(local_header + 26);
    uint16_t local_extra_len = read_le16(local_header + 28);
    uint32_t data_offset = entry.local_header_offset + sizeof(local_header) + local_name_len + local_extra_len;
    if (data_offset + entry.compressed_size > file_size) {
      fclose(file);
      error = "ZIP 数据越界: " + relative_path;
      return false;
    }
    if (fseek(file, static_cast<long>(data_offset), SEEK_SET) != 0) {
      fclose(file);
      error = "无法定位 ZIP 数据";
      return false;
    }

    FILE *out = fopen(output_path.c_str(), "wb");
    if (out == nullptr) {
      fclose(file);
      error = "无法写入文件: " + relative_path;
      return false;
    }

    bool ok = true;
    std::string entry_error;
    uLong crc = crc32(0L, Z_NULL, 0);
    uint32_t written_size = 0;

    if (entry.method == 0) {
      uint8_t buffer[2048];
      uint32_t remaining = entry.compressed_size;
      while (remaining > 0) {
        size_t chunk_size = std::min<size_t>(sizeof(buffer), remaining);
        if (!read_exact(file, buffer, chunk_size)) {
          entry_error = "ZIP 数据读取失败";
          ok = false;
          break;
        }
        if (fwrite(buffer, 1, chunk_size, out) != chunk_size) {
          entry_error = "文件写入失败";
          ok = false;
          break;
        }
        crc = crc32(crc, buffer, chunk_size);
        written_size += chunk_size;
        remaining -= static_cast<uint32_t>(chunk_size);
        App.feed_wdt();
        yield();
      }
    } else if (entry.method == 8) {
      uint8_t in_buffer[2048];
      uint8_t out_buffer[2048];
      uint32_t remaining = entry.compressed_size;
      z_stream stream{};
      int ret = inflateInit2(&stream, -MAX_WBITS);
      if (ret != Z_OK) {
        entry_error = "zlib 初始化失败";
        ok = false;
      }

      while (ok) {
        if (stream.avail_in == 0 && remaining > 0) {
          size_t chunk_size = std::min<size_t>(sizeof(in_buffer), remaining);
          if (!read_exact(file, in_buffer, chunk_size)) {
            entry_error = "ZIP 数据读取失败";
            ok = false;
            break;
          }
          stream.next_in = in_buffer;
          stream.avail_in = chunk_size;
          remaining -= static_cast<uint32_t>(chunk_size);
        }
        if (stream.avail_in == 0 && remaining == 0) {
          entry_error = "ZIP 压缩数据提前结束";
          ok = false;
          break;
        }

        stream.next_out = out_buffer;
        stream.avail_out = sizeof(out_buffer);
        ret = inflate(&stream, Z_NO_FLUSH);
        size_t produced = sizeof(out_buffer) - stream.avail_out;
        if (produced > 0) {
          if (fwrite(out_buffer, 1, produced, out) != produced) {
            entry_error = "文件写入失败";
            ok = false;
            break;
          }
          crc = crc32(crc, out_buffer, produced);
          written_size += static_cast<uint32_t>(produced);
        }
        if (ret == Z_STREAM_END) {
          break;
        }
        if (ret == Z_OK) {
          if (produced == 0 && stream.avail_in == 0 && remaining == 0) {
            entry_error = "ZIP 压缩数据提前结束";
            ok = false;
            break;
          }
          App.feed_wdt();
          yield();
          continue;
        }
        if (ret == Z_BUF_ERROR && produced == 0 && stream.avail_in == 0 && remaining == 0) {
          entry_error = "ZIP 压缩数据提前结束";
          ok = false;
          break;
        }
        if (ret != Z_OK) {
          entry_error = "ZIP 解压失败(" + std::to_string(ret) + ")";
          ok = false;
          break;
        }
        App.feed_wdt();
        yield();
      }

      inflateEnd(&stream);
    } else {
      entry_error = "暂不支持的压缩方式: " + std::to_string(entry.method);
      ok = false;
    }

    fclose(out);
    if (!ok) {
      remove_recursive_(output_path);
      fclose(file);
      error = entry_error + ": " + relative_path;
      return false;
    }
    if (written_size != entry.uncompressed_size) {
      remove_recursive_(output_path);
      fclose(file);
      error = "ZIP 解压尺寸不匹配: " + relative_path;
      return false;
    }
    if (static_cast<uint32_t>(crc) != entry.crc32) {
      remove_recursive_(output_path);
      fclose(file);
      error = "ZIP CRC 校验失败: " + relative_path;
      return false;
    }
  }

  fclose(file);
  return true;
}

bool AppStore::validate_stage_manifest_(const std::string &stage_dir, const RemoteApp &entry, std::string &error) const {
  std::string body;
  if (!this->read_file_text_(stage_dir + "/manifest.json", body, error)) {
    return false;
  }

  JsonDocument doc;
  DeserializationError json_error = deserializeJson(doc, body);
  if (json_error) {
    error = "manifest 解析失败";
    return false;
  }

  std::string manifest_id = doc["id"] | "";
  if (manifest_id.empty()) {
    error = "manifest 缺少 id";
    return false;
  }
  if (manifest_id != entry.id) {
    error = "manifest id 与索引不一致";
    return false;
  }

  std::string manifest_version = doc["version"] | "";
  if (!manifest_version.empty() && !entry.version.empty() && manifest_version != entry.version) {
    error = "manifest 版本与索引不一致";
    return false;
  }

  return true;
}

bool AppStore::read_file_text_(const std::string &path, std::string &body, std::string &error) const {
  std::ifstream input(path, std::ios::binary);
  if (!input.is_open()) {
    error = "无法读取文件";
    return false;
  }

  std::ostringstream stream;
  stream << input.rdbuf();
  if (!input.good() && !input.eof()) {
    error = "读取文件失败";
    return false;
  }

  body = stream.str();
  return true;
}

bool AppStore::write_file_text_(const std::string &path, const std::string &body, std::string &error) const {
  if (!ensure_parent_dir_(path)) {
    error = "无法创建目录";
    return false;
  }

  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output.is_open()) {
    error = "无法写入文件";
    return false;
  }

  output.write(body.data(), static_cast<std::streamsize>(body.size()));
  if (!output.good()) {
    error = "写入文件失败";
    return false;
  }
  return true;
}

void AppStore::ensure_store_dirs_() {
  mkdir_p_(STORE_ROOT);
  mkdir_p_(TMP_ROOT);
  mkdir_p_(TMP_PACKAGE_ROOT);
  mkdir_p_(TMP_STAGE_ROOT);
  mkdir_p_(BACKUP_ROOT);
  ESP_LOGD(TAG, "Ensured store dirs temp=%s data=%s", TMP_ROOT, STORE_ROOT);
}

void AppStore::scan_installed_apps_() {
  std::map<std::string, std::string> installed;
  std::map<std::string, std::string> installed_descriptions;
  DIR *dir = opendir(APP_ROOT);
  if (dir == nullptr) {
    LockGuard lock(this->mutex_);
    this->installed_versions_.swap(installed);
    this->installed_descriptions_.swap(installed_descriptions);
    this->ui_refresh_requested_ = true;
    return;
  }

  struct dirent *ent;
  while ((ent = readdir(dir)) != nullptr) {
    const char *name = ent->d_name;
    if (name == nullptr || strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
      continue;
    }

    const std::string app_dir = std::string(APP_ROOT) + "/" + name;
    if (!is_directory_(app_dir)) {
      continue;
    }

    std::string app_id = name;
    std::string version;
    std::string description;
    std::ifstream manifest(app_dir + "/manifest.json");
    if (manifest.is_open()) {
      JsonDocument doc;
      DeserializationError json_error = deserializeJson(doc, manifest);
      if (!json_error) {
        app_id = doc["id"] | app_id.c_str();
        version = doc["version"] | "";
        description = doc["description"] | "";
      }
      manifest.close();
    }
    installed[app_id] = version;
    installed_descriptions[app_id] = description;
  }
  closedir(dir);

  LockGuard lock(this->mutex_);
  this->installed_versions_.swap(installed);
  this->installed_descriptions_.swap(installed_descriptions);
  this->ui_refresh_requested_ = true;
  ESP_LOGD(TAG, "Scanned %u installed apps", static_cast<unsigned>(this->installed_versions_.size()));
}

bool AppStore::parse_index_document_(const std::string &body, std::vector<RemoteApp> &entries, std::string &error) const {
  JsonDocument doc;
  DeserializationError json_error = deserializeJson(doc, body);
  if (json_error) {
    error = json_error.c_str();
    return false;
  }

  if (!doc["apps"].is<JsonArray>()) {
    error = "缺少 apps 数组";
    return false;
  }

  std::string default_base_url = doc["base_url"] | "";
  if (default_base_url.empty()) {
    default_base_url = this->index_url_;
  }

  for (JsonObject app : doc["apps"].as<JsonArray>()) {
    bool platform_ok = true;
    if (app["platform"].is<JsonArray>()) {
      platform_ok = false;
      for (JsonVariant v : app["platform"].as<JsonArray>()) {
        const char *p = v.as<const char *>();
        if (p != nullptr && this->platform_ == p) {
          platform_ok = true;
          break;
        }
      }
    }
    if (!platform_ok) {
      continue;
    }

    RemoteApp entry;
    entry.id = app["id"] | "";
    entry.name = app["name"] | entry.id.c_str();
    entry.version = app["version"] | "";
    entry.description = app["description"] | "";
    entry.author = app["author"] | "";

    std::string app_base_url = app["base_url"] | default_base_url.c_str();
    entry.icon_url = resolve_url_(app_base_url, app["icon_url"] | "");

    std::string package_value = app["package_url"] | "";
    if (package_value.empty()) {
      package_value = app["package"] | "";
    }
    entry.package_url = resolve_url_(app_base_url, package_value);
    entry.package_sha256 = app["package_sha256"] | "";
    if (entry.package_sha256.empty()) {
      entry.package_sha256 = app["sha256"] | "";
    }

    if (!entry.id.empty() && !entry.version.empty() && !entry.package_url.empty()) {
      ESP_LOGD(TAG, "Index entry accepted: id=%s version=%s package=%s", entry.id.c_str(), entry.version.c_str(),
               entry.package_url.c_str());
      entries.push_back(std::move(entry));
    }
  }

  return true;
}

AppStore::RemoteApp AppStore::find_entry_copy_(const std::string &app_id, bool *found) const {
  LockGuard lock(this->mutex_);
  for (const auto &entry : this->entries_) {
    if (entry.id == app_id) {
      if (found != nullptr) {
        *found = true;
      }
      return entry;
    }
  }
  if (found != nullptr) {
    *found = false;
  }
  return {};
}

std::string AppStore::installed_version_for_(const std::string &app_id) const {
  auto it = this->installed_versions_.find(app_id);
  if (it == this->installed_versions_.end()) {
    return "";
  }
  return it->second;
}

bool AppStore::is_installed_(const std::string &app_id) const { return this->installed_versions_.count(app_id) != 0; }

bool AppStore::is_upgrade_available_(const std::string &app_id, const std::string &remote_version) const {
  auto it = this->installed_versions_.find(app_id);
  if (it == this->installed_versions_.end()) {
    return false;
  }
  return !it->second.empty() && it->second != remote_version;
}

std::string AppStore::resolve_url_(const std::string &base_url, const std::string &value) {
  if (value.empty()) {
    return "";
  }
  if (value.rfind("http://", 0) == 0 || value.rfind("https://", 0) == 0) {
    return value;
  }
  if (base_url.empty()) {
    return value;
  }

  std::string prefix = base_url;
  size_t slash = prefix.rfind('/');
  if (slash != std::string::npos) {
    prefix = prefix.substr(0, slash + 1);
  } else if (!prefix.empty() && prefix.back() != '/') {
    prefix.push_back('/');
  }
  return prefix + value;
}

std::string AppStore::normalize_relative_path_(const std::string &path) {
  if (path.empty()) {
    return "";
  }

  std::string normalized = path;
  for (auto &ch : normalized) {
    if (ch == '\\') {
      ch = '/';
    }
  }
  if (normalized.front() == '/') {
    return "";
  }

  std::string result;
  size_t start = 0;
  while (start < normalized.size()) {
    size_t end = normalized.find('/', start);
    if (end == std::string::npos) {
      end = normalized.size();
    }
    std::string segment = normalized.substr(start, end - start);
    start = end + 1;

    if (segment.empty() || segment == ".") {
      continue;
    }
    if (segment == "..") {
      return "";
    }
    if (!result.empty()) {
      result.push_back('/');
    }
    result += segment;
  }
  return result;
}

bool AppStore::mkdir_p_(const std::string &path) {
  if (path.empty() || path == ".") {
    return true;
  }
  if (path_exists_(path)) {
    return is_directory_(path);
  }

  std::string current;
  if (!path.empty() && path.front() == '/') {
    current = "/";
  }

  size_t start = current == "/" ? 1 : 0;
  while (start <= path.size()) {
    size_t end = path.find('/', start);
    std::string segment = path.substr(start, end == std::string::npos ? std::string::npos : end - start);
    if (!segment.empty()) {
      if (!current.empty() && current.back() != '/') {
        current.push_back('/');
      }
      current += segment;
      if (!path_exists_(current) && mkdir(current.c_str(), 0777) != 0) {
        return false;
      }
    }
    if (end == std::string::npos) {
      break;
    }
    start = end + 1;
  }
  return true;
}

bool AppStore::ensure_parent_dir_(const std::string &path) { return mkdir_p_(dirname_of(path)); }

bool AppStore::remove_recursive_(const std::string &path) {
  struct stat st;
  if (stat(path.c_str(), &st) != 0) {
    return true;
  }

  if (!S_ISDIR(st.st_mode)) {
    return unlink(path.c_str()) == 0;
  }

  DIR *dir = opendir(path.c_str());
  if (dir == nullptr) {
    return false;
  }

  bool ok = true;
  struct dirent *ent;
  while ((ent = readdir(dir)) != nullptr) {
    const char *name = ent->d_name;
    if (name == nullptr || strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
      continue;
    }
    if (!remove_recursive_(path + "/" + name)) {
      ok = false;
      break;
    }
  }
  closedir(dir);
  if (!ok) {
    return false;
  }
  return rmdir(path.c_str()) == 0;
}

bool AppStore::path_exists_(const std::string &path) {
  struct stat st;
  return stat(path.c_str(), &st) == 0;
}

bool AppStore::is_directory_(const std::string &path) {
  struct stat st;
  return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

void AppStore::set_status_(const std::string &message, bool ui_refresh) {
  LockGuard lock(this->mutex_);
  this->status_message_ = message;
  if (ui_refresh) {
    this->ui_refresh_requested_ = true;
  }
  ESP_LOGD(TAG, "Status: %s", message.c_str());
}

void AppStore::queue_notification_(const std::string &message, int delay_ms) {
  LockGuard lock(this->mutex_);
  this->notification_requested_ = true;
  this->notification_message_ = message;
  this->notification_delay_ms_ = delay_ms;
  this->ui_refresh_requested_ = true;
  ESP_LOGI(TAG, "Notification queued: %s", message.c_str());
}

void AppStore::mark_external_apps_refresh_() {
  LockGuard lock(this->mutex_);
  this->external_apps_refresh_requested_ = true;
  this->ui_refresh_requested_ = true;
}

}  // namespace app_store
}  // namespace esphome
