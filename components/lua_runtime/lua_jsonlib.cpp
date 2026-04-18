#ifndef LUA_RUNTIME_STUB

#include <string>

#include <ArduinoJson.h>

#include "lua.hpp"
#include "lua_json_util.h"

namespace {

static int lua_json_encode(lua_State *L) {
  JsonDocument doc;
  std::string err;
  JsonVariant root = doc.to<JsonVariant>();
  if (!esphome::lua_runtime::lua_value_to_json(L, 1, root, 0, err)) {
    return luaL_error(L, "json.encode failed: %s", err.c_str());
  }
  if (doc.overflowed()) {
    return luaL_error(L, "json.encode failed: out of memory");
  }

  std::string out;
  bool pretty = lua_gettop(L) >= 2 && lua_toboolean(L, 2) != 0;
  if (pretty) {
    serializeJsonPretty(doc, out);
  } else {
    serializeJson(doc, out);
  }
  lua_pushlstring(L, out.c_str(), out.size());
  return 1;
}

static int lua_json_decode(lua_State *L) {
  size_t len = 0;
  const char *json = luaL_checklstring(L, 1, &len);

  JsonDocument doc;
  DeserializationError rc = deserializeJson(doc, json, len);
  if (rc) {
    return luaL_error(L, "json.decode failed: %s", rc.c_str());
  }
  if (doc.overflowed()) {
    return luaL_error(L, "json.decode failed: out of memory");
  }

  std::string err;
  if (!esphome::lua_runtime::json_value_to_lua(L, doc.as<JsonVariantConst>(), 0, err)) {
    return luaL_error(L, "json.decode failed: %s", err.c_str());
  }
  return 1;
}

static const luaL_Reg JSON_FUNCS[] = {
    {"encode", lua_json_encode},
    {"decode", lua_json_decode},
    {"stringify", lua_json_encode},
    {"parse", lua_json_decode},
    {nullptr, nullptr},
};

}  // namespace

extern "C" int luaopen_json(lua_State *L) {
  luaL_newlib(L, JSON_FUNCS);
  esphome::lua_runtime::push_lua_json_null(L);
  lua_setfield(L, -2, "null");
  return 1;
}

#endif  // LUA_RUNTIME_STUB



