#include "lua_manifest.h"

#include <cctype>
#include <cstdio>
#include <cstring>

namespace esphome {
namespace lua_runtime {

namespace {

// 在 JSON 文本里找 "key" 对应的字符串值。manifest 是 flat 对象且字符串不含
// 转义字符，所以直接做引号匹配即可（同 app_display_name 的既有做法）。
bool find_string_value(const std::string &body, const char *key, std::string &out) {
  // 匹配 "key"（带引号），避免撞上其他键的子串。
  std::string needle = std::string("\"") + key + "\"";
  size_t key_pos = body.find(needle);
  if (key_pos == std::string::npos) return false;

  size_t colon = body.find(':', key_pos + needle.size());
  if (colon == std::string::npos) return false;

  size_t quote = body.find('"', colon + 1);
  if (quote == std::string::npos) return false;

  size_t end = quote + 1;
  while (end < body.size() && body[end] != '"') {
    if (body[end] == '\\' && end + 1 < body.size()) end++;  // 跳过转义字符
    end++;
  }
  if (end > body.size()) return false;
  out = body.substr(quote + 1, end - quote - 1);
  return true;
}

// 找 "key" 对应的整数值（数字字面量，不带引号）。
bool find_int_value(const std::string &body, const char *key, int &out) {
  std::string needle = std::string("\"") + key + "\"";
  size_t key_pos = body.find(needle);
  if (key_pos == std::string::npos) return false;

  size_t colon = body.find(':', key_pos + needle.size());
  if (colon == std::string::npos) return false;

  size_t pos = colon + 1;
  while (pos < body.size() && (body[pos] == ' ' || body[pos] == '\t')) pos++;

  bool negative = false;
  if (pos < body.size() && body[pos] == '-') {
    negative = true;
    pos++;
  }
  if (pos >= body.size() || !std::isdigit(static_cast<unsigned char>(body[pos]))) return false;

  long value = 0;
  while (pos < body.size() && std::isdigit(static_cast<unsigned char>(body[pos]))) {
    value = value * 10 + (body[pos] - '0');
    if (value > 1000000) return false;  // 防御性上限
    pos++;
  }
  out = static_cast<int>(negative ? -value : value);
  return true;
}

}  // namespace

LuaManifest read_lua_manifest(const std::string &app_dir) {
  LuaManifest manifest;

  FILE *file = fopen((app_dir + "/manifest.json").c_str(), "r");
  if (file == nullptr) return manifest;

  char buffer[2048];
  size_t len = fread(buffer, 1, sizeof(buffer) - 1, file);
  buffer[len] = '\0';
  fclose(file);

  std::string body(buffer, len);
  find_string_value(body, "name", manifest.name);
  find_string_value(body, "id", manifest.id);

  int min_api = 0;
  if (find_int_value(body, "min_api", min_api) && min_api > 0) {
    manifest.min_api = min_api;
  }
  int manifest_version = 0;
  if (find_int_value(body, "manifest_version", manifest_version) && manifest_version > 0) {
    manifest.manifest_version = manifest_version;
  }
  std::string type;
  if (find_string_value(body, "type", type) && !type.empty()) {
    manifest.type = type;
  }
  return manifest;
}

bool lua_manifest_api_supported(const LuaManifest &manifest, std::string &reason) {
  if (manifest.min_api <= LUA_API_LEVEL) return true;
  reason = "需要 API level " + std::to_string(manifest.min_api) + "，当前固件为 " +
           std::to_string(LUA_API_LEVEL) + "，请更新系统后再安装";
  return false;
}

}  // namespace lua_runtime
}  // namespace esphome
