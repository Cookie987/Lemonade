#include "lua_app.h"

#include <cstring>
#include <string>

#include <sys/stat.h>

#ifdef LUA_RUNTIME_STUB

namespace esphome {
namespace lua_runtime {

void register_app_api(lua_State *, const std::string &) {}

}  // namespace lua_runtime
}  // namespace esphome

#else

#include "lua.hpp"

namespace esphome {
namespace lua_runtime {

namespace {

static const char *APP_ROOT_PREFIX = "/sdcard/opt/";
static const char *DATA_ROOT = "/sdcard/var/opt";
static const char *APP_PACKAGE_KEY = "lua_app.package";
static const char *APP_DIR_KEY = "lua_app.dir";
static const char *APP_DATA_DIR_KEY = "lua_app.data_dir";

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

bool path_exists(const std::string &path) {
  struct stat st;
  return stat(path.c_str(), &st) == 0;
}

bool is_directory(const std::string &path) {
  struct stat st;
  return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool mkdir_p(const std::string &path) {
  if (path.empty() || path == ".") return true;
  if (path_exists(path)) return is_directory(path);

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

std::string normalize_relative_path(const std::string &path) {
  if (path.empty()) return "";

  std::string normalized = path;
  for (auto &ch : normalized) {
    if (ch == '\\') ch = '/';
  }
  if (!normalized.empty() && normalized.front() == '/') return "";

  std::string result;
  size_t start = 0;
  while (start < normalized.size()) {
    size_t end = normalized.find('/', start);
    if (end == std::string::npos) end = normalized.size();
    std::string segment = normalized.substr(start, end - start);
    start = end + 1;

    if (segment.empty() || segment == ".") continue;
    if (segment == "..") return "";
    if (!result.empty()) result.push_back('/');
    result += segment;
  }
  return result;
}

void push_string_or_nil(lua_State *L, const std::string &value) {
  if (value.empty()) {
    lua_pushnil(L);
  } else {
    lua_pushlstring(L, value.c_str(), value.size());
  }
}

std::string get_app_data_dir(lua_State *L) { return get_registry_string(L, APP_DATA_DIR_KEY); }

int l_app_package(lua_State *L) {
  push_string_or_nil(L, get_registry_string(L, APP_PACKAGE_KEY));
  return 1;
}

int l_app_dir(lua_State *L) {
  push_string_or_nil(L, get_registry_string(L, APP_DIR_KEY));
  return 1;
}

int l_app_data_dir(lua_State *L) {
  std::string data_dir = get_app_data_dir(L);
  if (!data_dir.empty()) {
    mkdir_p(data_dir);
  }
  push_string_or_nil(L, data_dir);
  return 1;
}

int l_app_data_path(lua_State *L) {
  std::string data_dir = get_app_data_dir(L);
  if (data_dir.empty()) {
    lua_pushnil(L);
    return 1;
  }

  size_t len = 0;
  const char *raw = luaL_checklstring(L, 1, &len);
  std::string normalized = normalize_relative_path(std::string(raw, len));
  if (normalized.empty()) {
    lua_pushnil(L);
    return 1;
  }

  mkdir_p(data_dir);
  std::string full_path = data_dir + "/" + normalized;
  lua_pushlstring(L, full_path.c_str(), full_path.size());
  return 1;
}

int l_app_mkdir(lua_State *L) {
  std::string data_dir = get_app_data_dir(L);
  if (data_dir.empty()) {
    lua_pushboolean(L, 0);
    return 1;
  }

  std::string target = data_dir;
  if (lua_gettop(L) >= 1 && !lua_isnoneornil(L, 1)) {
    size_t len = 0;
    const char *raw = luaL_checklstring(L, 1, &len);
    std::string normalized = normalize_relative_path(std::string(raw, len));
    if (normalized.empty()) {
      lua_pushboolean(L, 0);
      return 1;
    }
    target += "/" + normalized;
  }

  lua_pushboolean(L, mkdir_p(target) ? 1 : 0);
  return 1;
}

void set_app_context(lua_State *L, const std::string &script_path) {
  std::string package_name;
  std::string app_dir;
  std::string data_dir;

  if (starts_with(script_path, APP_ROOT_PREFIX)) {
    std::string relative = script_path.substr(strlen(APP_ROOT_PREFIX));
    size_t slash = relative.find('/');
    if (slash != std::string::npos && slash > 0) {
      package_name = relative.substr(0, slash);
      app_dir = std::string(APP_ROOT_PREFIX) + package_name;
      data_dir = std::string(DATA_ROOT) + "/" + package_name + "/data";
    }
  }

  set_registry_string(L, APP_PACKAGE_KEY, package_name);
  set_registry_string(L, APP_DIR_KEY, app_dir);
  set_registry_string(L, APP_DATA_DIR_KEY, data_dir);
}

}  // namespace

void register_app_api(lua_State *L, const std::string &script_path) {
  set_app_context(L, script_path);

  std::string package_name = get_registry_string(L, APP_PACKAGE_KEY);
  std::string app_dir = get_registry_string(L, APP_DIR_KEY);
  std::string data_dir = get_registry_string(L, APP_DATA_DIR_KEY);
  if (!data_dir.empty()) {
    mkdir_p(data_dir);
  }

  lua_newtable(L);
  lua_pushcfunction(L, l_app_package);
  lua_setfield(L, -2, "package");
  lua_pushcfunction(L, l_app_dir);
  lua_setfield(L, -2, "dir");
  lua_pushcfunction(L, l_app_data_dir);
  lua_setfield(L, -2, "data_dir");
  lua_pushcfunction(L, l_app_data_path);
  lua_setfield(L, -2, "data_path");
  lua_pushcfunction(L, l_app_mkdir);
  lua_setfield(L, -2, "mkdir");
  lua_setglobal(L, "app");

  push_string_or_nil(L, package_name);
  lua_setglobal(L, "APP_PACKAGE");
  push_string_or_nil(L, app_dir);
  lua_setglobal(L, "APP_DIR");
  push_string_or_nil(L, data_dir);
  lua_setglobal(L, "APP_DATA_DIR");
}

}  // namespace lua_runtime
}  // namespace esphome

#endif  // LUA_RUNTIME_STUB
