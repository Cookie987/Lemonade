#include "lua_fskv.h"

#include <fstream>
#include <cstring>
#include <new>
#include <sstream>
#include <string>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

#include "esphome/core/log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#ifdef LUA_RUNTIME_STUB

namespace esphome {
namespace lua_runtime {

void register_fskv_api(lua_State *, const std::string &) {}

}  // namespace lua_runtime
}  // namespace esphome

#else

#include <ArduinoJson.h>

#include "lua.hpp"
#include "lua_json_util.h"

namespace esphome {
namespace lua_runtime {

namespace {

static const char *TAG = "lua_fskv";
static const char *APP_ROOT_PREFIX = "/sdcard/opt/";
static const char *DATA_ROOT = "/sdcard/var/opt";
static const char *FSKV_PACKAGE_KEY = "lua_fskv.package";
static const char *FSKV_DATA_DIR_KEY = "lua_fskv.data_dir";
static const char *FSKV_FILE_KEY = "lua_fskv.file";
static const char *FSKV_ITER_MT = "lua_fskv.iter";
static constexpr size_t FSKV_MAX_VALUE_BYTES = 4095;

static SemaphoreHandle_t g_fskv_lock = nullptr;

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

struct FskvIter {
  std::vector<std::string> keys;
  size_t index{0};
};

void ensure_fskv_lock() {
  if (g_fskv_lock == nullptr) {
    g_fskv_lock = xSemaphoreCreateMutex();
  }
}

void set_registry_string(lua_State *L, const char *key, const std::string &value) {
  lua_pushlightuserdata(L, (void *) key);
  lua_pushlstring(L, value.c_str(), value.size());
  lua_settable(L, LUA_REGISTRYINDEX);
}

std::string get_registry_string(lua_State *L, const char *key) {
  lua_pushlightuserdata(L, (void *) key);
  lua_gettable(L, LUA_REGISTRYINDEX);
  size_t len = 0;
  const char *value = lua_tolstring(L, -1, &len);
  std::string out = value != nullptr ? std::string(value, len) : std::string();
  lua_pop(L, 1);
  return out;
}

bool starts_with(const std::string &value, const std::string &prefix) {
  return value.size() >= prefix.size() && value.compare(0, prefix.size(), prefix) == 0;
}

std::string dirname_of(const std::string &path) {
  size_t pos = path.rfind('/');
  if (pos == std::string::npos) return ".";
  if (pos == 0) return "/";
  return path.substr(0, pos);
}

bool path_exists(const std::string &path) {
  struct stat st;
  return stat(path.c_str(), &st) == 0;
}

bool mkdir_p(const std::string &path) {
  if (path.empty() || path == ".") return true;
  if (path_exists(path)) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
  }

  std::string current;
  if (!path.empty() && path.front() == '/') current = "/";

  size_t start = current == "/" ? 1 : 0;
  while (start <= path.size()) {
    size_t end = path.find('/', start);
    std::string segment = path.substr(start, end == std::string::npos ? std::string::npos : end - start);
    if (!segment.empty()) {
      if (!current.empty() && current.back() != '/') current.push_back('/');
      current += segment;
      if (!path_exists(current) && mkdir(current.c_str(), 0777) != 0) {
        return false;
      }
    }
    if (end == std::string::npos) break;
    start = end + 1;
  }
  return true;
}

bool ensure_parent_dir(const std::string &path) { return mkdir_p(dirname_of(path)); }

bool read_file_text(const std::string &path, std::string &body, std::string &error) {
  std::ifstream input(path, std::ios::binary);
  if (!input.is_open()) {
    error = "open failed";
    return false;
  }

  std::ostringstream stream;
  stream << input.rdbuf();
  if (!input.good() && !input.eof()) {
    error = "read failed";
    return false;
  }
  body = stream.str();
  return true;
}

bool write_file_text(const std::string &path, const std::string &body, std::string &error) {
  if (!ensure_parent_dir(path)) {
    error = "mkdir failed";
    return false;
  }

  const std::string tmp_path = path + ".tmp";
  {
    std::ofstream output(tmp_path, std::ios::binary | std::ios::trunc);
    if (!output.is_open()) {
      error = "open tmp failed";
      return false;
    }
    output.write(body.data(), static_cast<std::streamsize>(body.size()));
    if (!output.good()) {
      error = "write failed";
      output.close();
      unlink(tmp_path.c_str());
      return false;
    }
  }

  unlink(path.c_str());
  if (rename(tmp_path.c_str(), path.c_str()) != 0) {
    error = "rename failed";
    unlink(tmp_path.c_str());
    return false;
  }
  return true;
}

bool load_store(const std::string &path, JsonDocument &doc, std::string &error, bool allow_missing = false) {
  if (!path_exists(path)) {
    if (!allow_missing) {
      error = "store missing";
      return false;
    }
    doc.to<JsonObject>();
    return true;
  }

  std::string body;
  if (!read_file_text(path, body, error)) {
    return false;
  }
  if (body.empty()) {
    error = "store empty";
    return false;
  }

  DeserializationError rc = deserializeJson(doc, body);
  if (rc) {
    error = rc.c_str();
    return false;
  }
  if (!doc.is<JsonObject>()) {
    error = "store root must be object";
    return false;
  }
  return true;
}

bool save_store(const std::string &path, JsonDocument &doc, std::string &error) {
  std::string body;
  serializeJson(doc, body);
  return write_file_text(path, body, error);
}

bool is_supported_root_value(lua_State *L, int idx) {
  const int type = lua_type(L, idx);
  return type == LUA_TSTRING || type == LUA_TNUMBER || type == LUA_TBOOLEAN || type == LUA_TTABLE;
}

bool lua_fskv_value_to_json(lua_State *L, int idx, JsonVariant out, std::string &error) {
  if (!is_supported_root_value(L, idx)) {
    error = "unsupported value type";
    return false;
  }

  JsonDocument temp;
  JsonVariant root = temp.to<JsonVariant>();
  if (!lua_value_to_json(L, idx, root, 0, error)) {
    return false;
  }

  std::string encoded;
  serializeJson(temp, encoded);
  if (encoded.size() > FSKV_MAX_VALUE_BYTES) {
    error = "value too large";
    return false;
  }

  out.set(temp.as<JsonVariantConst>());
  return true;
}

bool get_fskv_context(lua_State *L, std::string &package_name, std::string &data_dir, std::string &file_path) {
  package_name = get_registry_string(L, FSKV_PACKAGE_KEY);
  data_dir = get_registry_string(L, FSKV_DATA_DIR_KEY);
  file_path = get_registry_string(L, FSKV_FILE_KEY);
  return !package_name.empty() && !data_dir.empty() && !file_path.empty();
}

bool get_fskv_key(lua_State *L, int idx, std::string &key) {
  if (!lua_isstring(L, idx)) {
    return false;
  }
  size_t len = 0;
  const char *value = lua_tolstring(L, idx, &len);
  if (value == nullptr || len == 0) {
    return false;
  }
  key.assign(value, len);
  return true;
}

bool ensure_store_ready(lua_State *L, std::string &package_name, std::string &data_dir, std::string &file_path,
                        bool create_if_missing, std::string &error) {
  if (!get_fskv_context(L, package_name, data_dir, file_path)) {
    error = "app context unavailable";
    return false;
  }
  if (!mkdir_p(data_dir)) {
    error = "mkdir failed";
    return false;
  }
  if (create_if_missing && !path_exists(file_path)) {
    JsonDocument doc;
    doc.to<JsonObject>();
    if (!save_store(file_path, doc, error)) {
      return false;
    }
  }
  return true;
}

size_t file_size_or_zero(const std::string &path) {
  struct stat st;
  if (stat(path.c_str(), &st) != 0) return 0;
  if (st.st_size < 0) return 0;
  return static_cast<size_t>(st.st_size);
}

size_t volume_total_or_zero(const std::string &path) {
  struct statvfs vfs;
  if (statvfs(path.c_str(), &vfs) != 0) return 0;
  return static_cast<size_t>(vfs.f_frsize) * static_cast<size_t>(vfs.f_blocks);
}

int l_fskv_init(lua_State *L) {
  ensure_fskv_lock();
  LockGuard lock(g_fskv_lock);

  std::string package_name;
  std::string data_dir;
  std::string file_path;
  std::string error;
  bool ok = ensure_store_ready(L, package_name, data_dir, file_path, true, error);
  if (!ok) {
    ESP_LOGW(TAG, "init failed: %s", error.c_str());
  }
  lua_pushboolean(L, ok ? 1 : 0);
  return 1;
}

int l_fskv_set(lua_State *L) {
  std::string key;
  if (!get_fskv_key(L, 1, key) || !is_supported_root_value(L, 2)) {
    lua_pushboolean(L, 0);
    return 1;
  }

  ensure_fskv_lock();
  LockGuard lock(g_fskv_lock);

  std::string package_name;
  std::string data_dir;
  std::string file_path;
  std::string error;
  if (!ensure_store_ready(L, package_name, data_dir, file_path, true, error)) {
    ESP_LOGW(TAG, "set failed for %s: %s", key.c_str(), error.c_str());
    lua_pushboolean(L, 0);
    return 1;
  }

  JsonDocument doc;
  if (!load_store(file_path, doc, error, true)) {
    ESP_LOGW(TAG, "set load failed for %s: %s", key.c_str(), error.c_str());
    lua_pushboolean(L, 0);
    return 1;
  }

  JsonObject root = doc.as<JsonObject>();
  JsonVariant slot = root[key.c_str()];
  if (!lua_fskv_value_to_json(L, 2, slot, error)) {
    ESP_LOGW(TAG, "set encode failed for %s: %s", key.c_str(), error.c_str());
    lua_pushboolean(L, 0);
    return 1;
  }

  bool ok = save_store(file_path, doc, error);
  if (!ok) {
    ESP_LOGW(TAG, "set save failed for %s: %s", key.c_str(), error.c_str());
  }
  lua_pushboolean(L, ok ? 1 : 0);
  return 1;
}

int l_fskv_sett(lua_State *L) {
  std::string key;
  std::string subkey;
  if (!get_fskv_key(L, 1, key) || !get_fskv_key(L, 2, subkey)) {
    lua_pushboolean(L, 0);
    return 1;
  }

  const bool deleting = lua_gettop(L) < 3 || lua_isnoneornil(L, 3);
  if (!deleting && !is_supported_root_value(L, 3)) {
    lua_pushboolean(L, 0);
    return 1;
  }

  ensure_fskv_lock();
  LockGuard lock(g_fskv_lock);

  std::string package_name;
  std::string data_dir;
  std::string file_path;
  std::string error;
  if (!ensure_store_ready(L, package_name, data_dir, file_path, true, error)) {
    ESP_LOGW(TAG, "sett failed for %s.%s: %s", key.c_str(), subkey.c_str(), error.c_str());
    lua_pushboolean(L, 0);
    return 1;
  }

  JsonDocument doc;
  if (!load_store(file_path, doc, error, true)) {
    ESP_LOGW(TAG, "sett load failed for %s.%s: %s", key.c_str(), subkey.c_str(), error.c_str());
    lua_pushboolean(L, 0);
    return 1;
  }

  JsonObject root = doc.as<JsonObject>();
  if (deleting) {
    JsonVariant slot = root[key.c_str()];
    if (slot.is<JsonObject>()) {
      JsonObject table_obj = slot.as<JsonObject>();
      table_obj.remove(subkey.c_str());
    }
  } else {
    JsonVariant slot = root[key.c_str()];
    JsonObject table_obj = slot.is<JsonObject>() ? slot.as<JsonObject>() : slot.to<JsonObject>();
    JsonVariant member = table_obj[subkey.c_str()];
    if (!lua_fskv_value_to_json(L, 3, member, error)) {
      ESP_LOGW(TAG, "sett encode failed for %s.%s: %s", key.c_str(), subkey.c_str(), error.c_str());
      lua_pushboolean(L, 0);
      return 1;
    }
  }

  bool ok = save_store(file_path, doc, error);
  if (!ok) {
    ESP_LOGW(TAG, "sett save failed for %s.%s: %s", key.c_str(), subkey.c_str(), error.c_str());
  }
  lua_pushboolean(L, ok ? 1 : 0);
  return 1;
}

int l_fskv_get(lua_State *L) {
  std::string key;
  if (!get_fskv_key(L, 1, key)) {
    lua_pushnil(L);
    return 1;
  }

  ensure_fskv_lock();
  LockGuard lock(g_fskv_lock);

  std::string package_name;
  std::string data_dir;
  std::string file_path;
  std::string error;
  if (!ensure_store_ready(L, package_name, data_dir, file_path, false, error)) {
    lua_pushnil(L);
    return 1;
  }

  JsonDocument doc;
  if (!load_store(file_path, doc, error, true)) {
    ESP_LOGW(TAG, "get load failed for %s: %s", key.c_str(), error.c_str());
    lua_pushnil(L);
    return 1;
  }

  JsonObjectConst root = doc.as<JsonObjectConst>();
  JsonVariantConst value = root[key.c_str()];
  if (value.isUnbound() || value.isNull()) {
    lua_pushnil(L);
    return 1;
  }

  if (lua_gettop(L) >= 2 && !lua_isnoneornil(L, 2) && lua_isstring(L, 2)) {
    size_t len = 0;
    const char *subkey = lua_tolstring(L, 2, &len);
    if (!value.is<JsonObjectConst>() || subkey == nullptr || len == 0) {
      lua_pushnil(L);
      return 1;
    }
    JsonObjectConst table_obj = value.as<JsonObjectConst>();
    std::string subkey_str(subkey, len);
    value = table_obj[subkey_str.c_str()];
    if (value.isUnbound() || value.isNull()) {
      lua_pushnil(L);
      return 1;
    }
  }

  if (!json_value_to_lua(L, value, 0, error)) {
    ESP_LOGW(TAG, "get decode failed for %s: %s", key.c_str(), error.c_str());
    lua_pushnil(L);
  }
  return 1;
}

int l_fskv_del(lua_State *L) {
  std::string key;
  if (!get_fskv_key(L, 1, key)) {
    lua_pushboolean(L, 0);
    return 1;
  }

  ensure_fskv_lock();
  LockGuard lock(g_fskv_lock);

  std::string package_name;
  std::string data_dir;
  std::string file_path;
  std::string error;
  if (!ensure_store_ready(L, package_name, data_dir, file_path, true, error)) {
    ESP_LOGW(TAG, "del failed for %s: %s", key.c_str(), error.c_str());
    lua_pushboolean(L, 0);
    return 1;
  }

  JsonDocument doc;
  if (!load_store(file_path, doc, error, true)) {
    ESP_LOGW(TAG, "del load failed for %s: %s", key.c_str(), error.c_str());
    lua_pushboolean(L, 0);
    return 1;
  }

  doc.as<JsonObject>().remove(key.c_str());
  bool ok = save_store(file_path, doc, error);
  if (!ok) {
    ESP_LOGW(TAG, "del save failed for %s: %s", key.c_str(), error.c_str());
  }
  lua_pushboolean(L, ok ? 1 : 0);
  return 1;
}

int l_fskv_clear(lua_State *L) {
  ensure_fskv_lock();
  LockGuard lock(g_fskv_lock);

  std::string package_name;
  std::string data_dir;
  std::string file_path;
  std::string error;
  if (!ensure_store_ready(L, package_name, data_dir, file_path, true, error)) {
    ESP_LOGW(TAG, "clear failed: %s", error.c_str());
    lua_pushboolean(L, 0);
    return 1;
  }

  JsonDocument doc;
  doc.to<JsonObject>();
  bool ok = save_store(file_path, doc, error);
  if (!ok) {
    ESP_LOGW(TAG, "clear save failed: %s", error.c_str());
  }
  lua_pushboolean(L, ok ? 1 : 0);
  return 1;
}

int l_fskv_iter_gc(lua_State *L) {
  auto *iter = static_cast<FskvIter *>(luaL_testudata(L, 1, FSKV_ITER_MT));
  if (iter != nullptr) {
    iter->~FskvIter();
  }
  return 0;
}

void ensure_iter_metatable(lua_State *L) {
  if (luaL_newmetatable(L, FSKV_ITER_MT)) {
    lua_pushcfunction(L, l_fskv_iter_gc);
    lua_setfield(L, -2, "__gc");
  }
  lua_pop(L, 1);
}

int l_fskv_iter(lua_State *L) {
  ensure_fskv_lock();
  LockGuard lock(g_fskv_lock);

  std::string package_name;
  std::string data_dir;
  std::string file_path;
  std::string error;
  if (!ensure_store_ready(L, package_name, data_dir, file_path, false, error)) {
    lua_pushnil(L);
    return 1;
  }

  JsonDocument doc;
  if (!load_store(file_path, doc, error, true)) {
    ESP_LOGW(TAG, "iter load failed: %s", error.c_str());
    lua_pushnil(L);
    return 1;
  }

  ensure_iter_metatable(L);
  void *mem = lua_newuserdatauv(L, sizeof(FskvIter), 0);
  auto *iter = new(mem) FskvIter();

  JsonObjectConst root = doc.as<JsonObjectConst>();
  iter->keys.reserve(root.size());
  for (JsonPairConst kv : root) {
    iter->keys.emplace_back(kv.key().c_str());
  }

  luaL_getmetatable(L, FSKV_ITER_MT);
  lua_setmetatable(L, -2);
  return 1;
}

int l_fskv_next(lua_State *L) {
  auto *iter = static_cast<FskvIter *>(luaL_testudata(L, 1, FSKV_ITER_MT));
  if (iter == nullptr || iter->index >= iter->keys.size()) {
    lua_pushnil(L);
    return 1;
  }

  const std::string &key = iter->keys[iter->index++];
  lua_pushlstring(L, key.c_str(), key.size());
  return 1;
}

int l_fskv_status(lua_State *L) {
  ensure_fskv_lock();
  LockGuard lock(g_fskv_lock);

  std::string package_name;
  std::string data_dir;
  std::string file_path;
  std::string error;
  if (!ensure_store_ready(L, package_name, data_dir, file_path, false, error)) {
    lua_pushinteger(L, 0);
    lua_pushinteger(L, 0);
    lua_pushinteger(L, 0);
    return 3;
  }

  JsonDocument doc;
  size_t count = 0;
  if (load_store(file_path, doc, error, true)) {
    count = doc.as<JsonObjectConst>().size();
  } else {
    ESP_LOGW(TAG, "status load failed: %s", error.c_str());
  }

  lua_pushinteger(L, static_cast<lua_Integer>(file_size_or_zero(file_path)));
  lua_pushinteger(L, static_cast<lua_Integer>(volume_total_or_zero(data_dir)));
  lua_pushinteger(L, static_cast<lua_Integer>(count));
  return 3;
}

void set_fskv_context(lua_State *L, const std::string &script_path) {
  std::string package_name;
  if (starts_with(script_path, APP_ROOT_PREFIX)) {
    std::string relative = script_path.substr(strlen(APP_ROOT_PREFIX));
    size_t slash = relative.find('/');
    if (slash != std::string::npos && slash > 0) {
      package_name = relative.substr(0, slash);
    }
  }

  std::string data_dir;
  std::string file_path;
  if (!package_name.empty()) {
    data_dir = std::string(DATA_ROOT) + "/" + package_name;
    file_path = data_dir + "/fskv.json";
  }

  set_registry_string(L, FSKV_PACKAGE_KEY, package_name);
  set_registry_string(L, FSKV_DATA_DIR_KEY, data_dir);
  set_registry_string(L, FSKV_FILE_KEY, file_path);
}

}  // namespace

void register_fskv_api(lua_State *L, const std::string &script_path) {
  set_fskv_context(L, script_path);

  lua_newtable(L);

  lua_pushcfunction(L, l_fskv_init);
  lua_setfield(L, -2, "init");
  lua_pushcfunction(L, l_fskv_set);
  lua_setfield(L, -2, "set");
  lua_pushcfunction(L, l_fskv_sett);
  lua_setfield(L, -2, "sett");
  lua_pushcfunction(L, l_fskv_get);
  lua_setfield(L, -2, "get");
  lua_pushcfunction(L, l_fskv_del);
  lua_setfield(L, -2, "del");
  lua_pushcfunction(L, l_fskv_clear);
  lua_setfield(L, -2, "clear");
  lua_pushcfunction(L, l_fskv_iter);
  lua_setfield(L, -2, "iter");
  lua_pushcfunction(L, l_fskv_next);
  lua_setfield(L, -2, "next");
  lua_pushcfunction(L, l_fskv_status);
  lua_setfield(L, -2, "status");

  lua_setglobal(L, "fskv");
}

}  // namespace lua_runtime
}  // namespace esphome

#endif  // LUA_RUNTIME_STUB
