#ifndef LUA_RUNTIME_STUB

#include <string>

#include <ArduinoJson.h>

#include "lua.hpp"

namespace {

static char JSON_NULL_SENTINEL = 0;
static const int JSON_MAX_DEPTH = 64;

static void push_json_null(lua_State *L) { lua_pushlightuserdata(L, &JSON_NULL_SENTINEL); }

static bool is_json_null(lua_State *L, int idx) {
  return lua_islightuserdata(L, idx) && lua_touserdata(L, idx) == &JSON_NULL_SENTINEL;
}

static bool is_array_table(lua_State *L, int idx, size_t *len) {
  idx = lua_absindex(L, idx);
  lua_Integer max_index = 0;
  size_t count = 0;

  lua_pushnil(L);
  while (lua_next(L, idx) != 0) {
    if (!lua_isinteger(L, -2)) {
      lua_pop(L, 2);
      return false;
    }
    lua_Integer key = lua_tointeger(L, -2);
    if (key < 1) {
      lua_pop(L, 2);
      return false;
    }
    if (key > max_index) max_index = key;
    count++;
    lua_pop(L, 1);
  }

  if (max_index > static_cast<lua_Integer>(count)) return false;
  *len = static_cast<size_t>(max_index);
  return true;
}

static bool lua_to_json(lua_State *L, int idx, JsonVariant out, int depth, std::string &err) {
  if (depth > JSON_MAX_DEPTH) {
    err = "too many nested levels";
    return false;
  }

  idx = lua_absindex(L, idx);
  const int t = lua_type(L, idx);
  switch (t) {
    case LUA_TNIL:
      out.set(nullptr);
      return true;
    case LUA_TBOOLEAN:
      out.set(lua_toboolean(L, idx) != 0);
      return true;
    case LUA_TNUMBER:
      if (lua_isinteger(L, idx)) {
        out.set(static_cast<int64_t>(lua_tointeger(L, idx)));
      } else {
        out.set(static_cast<double>(lua_tonumber(L, idx)));
      }
      return true;
    case LUA_TSTRING:
      out.set(lua_tostring(L, idx));
      return true;
    case LUA_TLIGHTUSERDATA:
      if (is_json_null(L, idx)) {
        out.set(nullptr);
        return true;
      }
      err = "unsupported lightuserdata";
      return false;
    case LUA_TTABLE: {
      size_t arr_len = 0;
      if (is_array_table(L, idx, &arr_len)) {
        JsonArray arr = out.to<JsonArray>();
        for (size_t i = 1; i <= arr_len; i++) {
          lua_geti(L, idx, static_cast<lua_Integer>(i));
          JsonVariant item = arr.add<JsonVariant>();
          if (!lua_to_json(L, -1, item, depth + 1, err)) {
            lua_pop(L, 1);
            return false;
          }
          lua_pop(L, 1);
        }
        return true;
      }

      JsonObject obj = out.to<JsonObject>();
      lua_pushnil(L);
      while (lua_next(L, idx) != 0) {
        std::string key;
        if (lua_type(L, -2) == LUA_TSTRING) {
          key = lua_tostring(L, -2);
        } else if (lua_type(L, -2) == LUA_TNUMBER) {
          size_t key_len = 0;
          const char *key_str = luaL_tolstring(L, -2, &key_len);
          key.assign(key_str, key_len);
          lua_pop(L, 1);
        } else {
          err = "object key must be string or number";
          lua_pop(L, 2);
          return false;
        }

        JsonVariant member = obj[key.c_str()];
        if (!lua_to_json(L, -1, member, depth + 1, err)) {
          lua_pop(L, 2);
          return false;
        }
        lua_pop(L, 1);
      }
      return true;
    }
    default:
      err = "unsupported lua type: " + std::string(lua_typename(L, t));
      return false;
  }
}

static bool json_to_lua(lua_State *L, JsonVariantConst in, int depth, std::string &err) {
  if (depth > JSON_MAX_DEPTH) {
    err = "too many nested levels";
    return false;
  }

  if (in.isNull()) {
    push_json_null(L);
    return true;
  }
  if (in.is<bool>()) {
    lua_pushboolean(L, in.as<bool>() ? 1 : 0);
    return true;
  }
  if (in.is<int64_t>()) {
    int64_t v = in.as<int64_t>();
    if (v >= static_cast<int64_t>(LUA_MININTEGER) && v <= static_cast<int64_t>(LUA_MAXINTEGER)) {
      lua_pushinteger(L, static_cast<lua_Integer>(v));
    } else {
      lua_pushnumber(L, static_cast<lua_Number>(v));
    }
    return true;
  }
  if (in.is<uint64_t>()) {
    uint64_t v = in.as<uint64_t>();
    if (v <= static_cast<uint64_t>(LUA_MAXINTEGER)) {
      lua_pushinteger(L, static_cast<lua_Integer>(v));
    } else {
      lua_pushnumber(L, static_cast<lua_Number>(v));
    }
    return true;
  }
  if (in.is<double>()) {
    lua_pushnumber(L, static_cast<lua_Number>(in.as<double>()));
    return true;
  }
  if (in.is<const char *>()) {
    const char *s = in.as<const char *>();
    lua_pushstring(L, s != nullptr ? s : "");
    return true;
  }
  if (in.is<JsonArrayConst>()) {
    JsonArrayConst arr = in.as<JsonArrayConst>();
    lua_createtable(L, static_cast<int>(arr.size()), 0);
    size_t i = 1;
    for (JsonVariantConst item : arr) {
      if (!json_to_lua(L, item, depth + 1, err)) return false;
      lua_seti(L, -2, static_cast<lua_Integer>(i));
      i++;
    }
    return true;
  }
  if (in.is<JsonObjectConst>()) {
    JsonObjectConst obj = in.as<JsonObjectConst>();
    lua_createtable(L, 0, static_cast<int>(obj.size()));
    for (JsonPairConst kv : obj) {
      if (!json_to_lua(L, kv.value(), depth + 1, err)) return false;
      lua_setfield(L, -2, kv.key().c_str());
    }
    return true;
  }

  err = "unsupported json value";
  return false;
}

static int lua_json_encode(lua_State *L) {
  JsonDocument doc;
  std::string err;
  JsonVariant root = doc.to<JsonVariant>();
  if (!lua_to_json(L, 1, root, 0, err)) {
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
  if (!json_to_lua(L, doc.as<JsonVariantConst>(), 0, err)) {
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
  push_json_null(L);
  lua_setfield(L, -2, "null");
  return 1;
}

#endif  // LUA_RUNTIME_STUB



