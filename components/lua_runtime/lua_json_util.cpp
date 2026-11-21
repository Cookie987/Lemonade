#ifndef LUA_RUNTIME_STUB

#include "lua_json_util.h"

namespace esphome {
namespace lua_runtime {

namespace {

char JSON_NULL_SENTINEL = 0;

bool is_array_table(lua_State *L, int idx, size_t *len) {
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

// Best-effort accounting of the bytes the encoder is about to copy into the
// document. Its only job is to refuse a payload that is already certain to be too
// large *before* it is copied, so a single huge Lua string (one big allocation)
// can no longer be materialized just to be rejected afterwards.
//
// The charges are deliberate lower bounds: every node costs at least two bytes of
// JSON ("1," / "null" / a separator), and a string costs exactly its length plus
// quotes. A lower bound can only let something through, never reject a value that
// would have fitted, so the exact limit stays with measureJson() in the caller.
class EncodeContext {
 public:
  explicit EncodeContext(const JsonEncodeOptions &options) : options_(options) {}

  bool consume(size_t bytes) {
    if (options_.max_bytes == 0) return true;
    if (bytes > options_.max_bytes - used_) return false;
    used_ += bytes;
    return true;
  }

  bool strict_table_keys() const { return options_.strict_table_keys; }

 private:
  const JsonEncodeOptions &options_;
  size_t used_{0};
};

// Smallest possible JSON contribution of a scalar or of a member key.
constexpr size_t JSON_NODE_MIN_BYTES = 2;

}  // namespace

void push_lua_json_null(lua_State *L) { lua_pushlightuserdata(L, &JSON_NULL_SENTINEL); }

bool is_lua_json_null(lua_State *L, int idx) {
  return lua_islightuserdata(L, idx) && lua_touserdata(L, idx) == &JSON_NULL_SENTINEL;
}

static bool lua_value_to_json_impl(lua_State *L, int idx, JsonVariant out, int depth, std::string &err,
                                   EncodeContext &ctx) {
  if (depth > LUA_JSON_MAX_DEPTH) {
    err = "too many nested levels";
    return false;
  }

  // Every level keeps the key/value pair of the table it walks on the Lua stack, so
  // a deep table relies on the stack growing while the iteration is in progress.
  // Reserve the worst case up front instead: that growth is what corrupted the
  // state (and crashed on lua_close) once nesting got deep enough.
  if (!lua_checkstack(L, 2 * (LUA_JSON_MAX_DEPTH - depth) + 8)) {
    err = "out of stack space";
    return false;
  }

  idx = lua_absindex(L, idx);
  const int t = lua_type(L, idx);
  switch (t) {
    case LUA_TNIL:
      if (!ctx.consume(JSON_NODE_MIN_BYTES)) {
        err = "value too large";
        return false;
      }
      out.set(nullptr);
      return true;
    case LUA_TBOOLEAN:
      if (!ctx.consume(JSON_NODE_MIN_BYTES)) {
        err = "value too large";
        return false;
      }
      out.set(lua_toboolean(L, idx) != 0);
      return true;
    case LUA_TNUMBER:
      if (!ctx.consume(JSON_NODE_MIN_BYTES)) {
        err = "value too large";
        return false;
      }
      if (lua_isinteger(L, idx)) {
        out.set(static_cast<int64_t>(lua_tointeger(L, idx)));
      } else {
        out.set(static_cast<double>(lua_tonumber(L, idx)));
      }
      return true;
    case LUA_TSTRING: {
      size_t len = 0;
      const char *value = lua_tolstring(L, idx, &len);
      // Reject before copying: this is the allocation that would otherwise blow up.
      if (!ctx.consume(len + 2)) {  // exact: the string plus its quotes
        err = "value too large";
        return false;
      }
      // JSON cannot carry a NUL, and silently truncating would corrupt the value.
      if (value != nullptr && memchr(value, '\0', len) != nullptr) {
        err = "string contains a NUL byte";
        return false;
      }
      out.set(value != nullptr ? value : "");
      return true;
    }
    case LUA_TLIGHTUSERDATA:
      if (is_lua_json_null(L, idx)) {
        if (!ctx.consume(JSON_NODE_MIN_BYTES)) {
          err = "value too large";
          return false;
        }
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
          if (!lua_value_to_json_impl(L, -1, item, depth + 1, err, ctx)) {
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
          if (ctx.strict_table_keys()) {
            // This table is not a contiguous 1..n array, so its numeric keys would
            // be stored as strings and come back as strings.
            err = "table is neither a contiguous array nor a string-keyed table";
            lua_pop(L, 2);
            return false;
          }
          size_t key_len = 0;
          const char *key_str = luaL_tolstring(L, -2, &key_len);
          key.assign(key_str, key_len);
          lua_pop(L, 1);
        } else {
          err = "object key must be string or number";
          lua_pop(L, 2);
          return false;
        }

        // The key plus its quotes: a lower bound, like every charge here.
        if (!ctx.consume(key.size() + 2)) {
          err = "value too large";
          lua_pop(L, 2);
          return false;
        }

        // to<JsonVariant>() creates the member and returns a variant bound to it.
        // Assigning obj[key] to a JsonVariant instead would only bind to an
        // already existing member and silently drop every new key.
        JsonVariant member = obj[key.c_str()].to<JsonVariant>();
        if (!lua_value_to_json_impl(L, -1, member, depth + 1, err, ctx)) {
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

bool lua_value_to_json(lua_State *L, int idx, JsonVariant out, int depth, std::string &err) {
  const JsonEncodeOptions options{};
  EncodeContext ctx(options);
  return lua_value_to_json_impl(L, idx, out, depth, err, ctx);
}

bool lua_value_to_json_ex(lua_State *L, int idx, JsonVariant out, const JsonEncodeOptions &options,
                          std::string &err) {
  EncodeContext ctx(options);
  return lua_value_to_json_impl(L, idx, out, 0, err, ctx);
}

bool json_value_to_lua(lua_State *L, JsonVariantConst in, int depth, std::string &err) {
  if (depth > LUA_JSON_MAX_DEPTH) {
    err = "too many nested levels";
    return false;
  }

  if (in.isNull()) {
    push_lua_json_null(L);
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
      if (!json_value_to_lua(L, item, depth + 1, err)) return false;
      lua_seti(L, -2, static_cast<lua_Integer>(i));
      i++;
    }
    return true;
  }
  if (in.is<JsonObjectConst>()) {
    JsonObjectConst obj = in.as<JsonObjectConst>();
    lua_createtable(L, 0, static_cast<int>(obj.size()));
    for (JsonPairConst kv : obj) {
      if (!json_value_to_lua(L, kv.value(), depth + 1, err)) return false;
      lua_setfield(L, -2, kv.key().c_str());
    }
    return true;
  }

  err = "unsupported json value";
  return false;
}

}  // namespace lua_runtime
}  // namespace esphome

#endif  // LUA_RUNTIME_STUB
