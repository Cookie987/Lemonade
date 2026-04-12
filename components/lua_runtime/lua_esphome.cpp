#include "lua_esphome.h"

#ifdef LUA_RUNTIME_STUB
struct lua_State;

namespace esphome {
namespace lua_runtime {

void register_esphome_api(lua_State *) {}

}  // namespace lua_runtime
}  // namespace esphome

#else

#include <array>
#include <unordered_map>

#include "esphome/components/rtttl/rtttl.h"
#include "esphome/components/switch/switch.h"
#include "esphome/core/application.h"

#include "lua.hpp"

namespace esphome {
namespace lua_runtime {

static std::unordered_map<std::string, rtttl::Rtttl *> g_rtttl_players;

void register_rtttl_player(const std::string &name, rtttl::Rtttl *player) {
  g_rtttl_players[name] = player;
}

static int lua_switch_get_state(lua_State *L) {
  const char *identifier = luaL_checkstring(L, 1);

#ifdef USE_SWITCH
  for (auto *sw : App.get_switches()) {
    if (sw == nullptr) continue;
    std::array<char, OBJECT_ID_MAX_LEN> object_id_buf{};
    auto object_id = sw->get_object_id_to(object_id_buf);
    if (object_id == identifier || sw->get_name() == identifier) {
      lua_pushboolean(L, sw->state);
      return 1;
    }
  }

  lua_pushnil(L);
  lua_pushfstring(L, "switch not found: %s", identifier);
  return 2;
#else
  lua_pushnil(L);
  lua_pushstring(L, "switch support is not enabled");
  return 2;
#endif
}

static int lua_rtttl_play(lua_State *L) {
  const char *player_name = luaL_checkstring(L, 1);
  const char *song = luaL_checkstring(L, 2);
  auto it = g_rtttl_players.find(player_name);
  if (it == g_rtttl_players.end() || it->second == nullptr) {
    return luaL_error(L, "rtttl player not found: %s", player_name);
  }

  it->second->play(song);
  return 0;
}

static int lua_rtttl_stop(lua_State *L) {
  const char *player_name = luaL_checkstring(L, 1);
  auto it = g_rtttl_players.find(player_name);
  if (it == g_rtttl_players.end() || it->second == nullptr) {
    return luaL_error(L, "rtttl player not found: %s", player_name);
  }

  it->second->stop();
  return 0;
}

static int lua_rtttl_is_playing(lua_State *L) {
  const char *player_name = luaL_checkstring(L, 1);
  auto it = g_rtttl_players.find(player_name);
  if (it == g_rtttl_players.end() || it->second == nullptr) {
    return luaL_error(L, "rtttl player not found: %s", player_name);
  }

  lua_pushboolean(L, it->second->is_playing());
  return 1;
}

static void register_switch_api(lua_State *L) {
  lua_newtable(L);  // switch

  lua_pushcfunction(L, lua_switch_get_state);
  lua_setfield(L, -2, "get_state");

  lua_setglobal(L, "switch");
}

static void register_rtttl_api(lua_State *L) {
  lua_newtable(L);  // rtttl

  lua_pushcfunction(L, lua_rtttl_play);
  lua_setfield(L, -2, "play");
  lua_pushcfunction(L, lua_rtttl_stop);
  lua_setfield(L, -2, "stop");
  lua_pushcfunction(L, lua_rtttl_is_playing);
  lua_setfield(L, -2, "is_playing");

  lua_setglobal(L, "rtttl");
}

void register_esphome_api(lua_State *L) {
  register_switch_api(L);
  register_rtttl_api(L);
}

}  // namespace lua_runtime
}  // namespace esphome

#endif
