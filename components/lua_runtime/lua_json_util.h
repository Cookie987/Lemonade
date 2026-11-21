#pragma once

#ifndef LUA_RUNTIME_STUB

#include <string>

#include <ArduinoJson.h>

#include "lua.hpp"

namespace esphome {
namespace lua_runtime {

constexpr int LUA_JSON_MAX_DEPTH = 64;

// Encoder limits. max_bytes = 0 means "unlimited"; the byte accounting is a lower
// bound used only to refuse an oversized payload before it is copied, the exact
// limit stays with the caller's own size check.
struct JsonEncodeOptions {
  size_t max_bytes{0};
  // Reject tables that would not survive a round trip: a table whose keys are not a
  // contiguous 1..n array and not all strings becomes a JSON object, and reading it
  // back yields string keys (t[1] would turn into nil). Off for plain encoding.
  bool strict_table_keys{false};
};

void push_lua_json_null(lua_State *L);
bool is_lua_json_null(lua_State *L, int idx);
bool lua_value_to_json(lua_State *L, int idx, JsonVariant out, int depth, std::string &err);
bool lua_value_to_json_ex(lua_State *L, int idx, JsonVariant out, const JsonEncodeOptions &options,
                          std::string &err);
bool json_value_to_lua(lua_State *L, JsonVariantConst in, int depth, std::string &err);

}  // namespace lua_runtime
}  // namespace esphome

#endif  // LUA_RUNTIME_STUB
