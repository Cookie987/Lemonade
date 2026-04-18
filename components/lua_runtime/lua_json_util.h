#pragma once

#ifndef LUA_RUNTIME_STUB

#include <string>

#include <ArduinoJson.h>

#include "lua.hpp"

namespace esphome {
namespace lua_runtime {

constexpr int LUA_JSON_MAX_DEPTH = 64;

void push_lua_json_null(lua_State *L);
bool is_lua_json_null(lua_State *L, int idx);
bool lua_value_to_json(lua_State *L, int idx, JsonVariant out, int depth, std::string &err);
bool json_value_to_lua(lua_State *L, JsonVariantConst in, int depth, std::string &err);

}  // namespace lua_runtime
}  // namespace esphome

#endif  // LUA_RUNTIME_STUB
