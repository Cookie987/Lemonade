#include "lua_esphome.h"

#ifdef LUA_RUNTIME_STUB
struct lua_State;

namespace esphome {
namespace lua_runtime {

void set_device_info(const std::string &, const std::string &, const std::string &, const std::string &) {}
void register_sd_mmc_card(const std::string &, esphome::sd_mmc_card::SdMmc *) {}
void register_media_player(const std::string &, esphome::media_player::MediaPlayer *) {}
void register_esphome_api(lua_State *) {}
void register_media_player(const std::string &, esphome::media_player::MediaPlayer *) {}

}  // namespace lua_runtime
}  // namespace esphome

#else

#include <array>
#include <unordered_map>

#include "esphome/core/defines.h"
#ifdef USE_MEDIA_PLAYER
#include "esphome/components/media_player/media_player.h"
#endif
#include "esphome/components/rtttl/rtttl.h"
#ifdef USE_SD_MMC_CARD
#include "esphome/components/sd_mmc_card/sd_mmc_card.h"
#endif
#include "esphome/components/switch/switch.h"
#include "esphome/core/application.h"

#include "lua.hpp"

namespace esphome {
namespace lua_runtime {

static std::unordered_map<std::string, rtttl::Rtttl *> g_rtttl_players;
#ifdef USE_MEDIA_PLAYER
static std::unordered_map<std::string, media_player::MediaPlayer *> g_media_players;
static std::unordered_map<std::string, std::string> g_media_last_urls;
#endif

void register_rtttl_player(const std::string &name, rtttl::Rtttl *player) {
  g_rtttl_players[name] = player;
}

void register_media_player(const std::string &name, media_player::MediaPlayer *player) {
#ifdef USE_MEDIA_PLAYER
  g_media_players[name] = player;
#else
  (void) name;
  (void) player;
#endif
}

#ifdef USE_MEDIA_PLAYER
static media_player::MediaPlayer *get_media_player(lua_State *L, const char *name) {
  auto it = g_media_players.find(name);
  if (it == g_media_players.end() || it->second == nullptr) {
    luaL_error(L, "media player not found: %s", name);
    return nullptr;
  }
  return it->second;
}
#endif

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

static int lua_media_player_play(lua_State *L) {
#ifdef USE_MEDIA_PLAYER
  const char *name = luaL_checkstring(L, 1);
  const char *url = luaL_checkstring(L, 2);
  auto *player = get_media_player(L, name);
  player->make_call().set_media_url(url).perform();
  g_media_last_urls[name] = url;
  return 0;
#else
  return luaL_error(L, "media_player support is not enabled");
#endif
}

#ifdef USE_MEDIA_PLAYER
static int lua_media_player_command(lua_State *L, media_player::MediaPlayerCommand command) {
  const char *name = luaL_checkstring(L, 1);
  auto *player = get_media_player(L, name);
  player->make_call().set_command(command).perform();
  return 0;
}
#endif

static int lua_media_player_pause(lua_State *L) {
#ifdef USE_MEDIA_PLAYER
  return lua_media_player_command(L, media_player::MEDIA_PLAYER_COMMAND_PAUSE);
#else
  return luaL_error(L, "media_player support is not enabled");
#endif
}

static int lua_media_player_stop(lua_State *L) {
#ifdef USE_MEDIA_PLAYER
  return lua_media_player_command(L, media_player::MEDIA_PLAYER_COMMAND_STOP);
#else
  return luaL_error(L, "media_player support is not enabled");
#endif
}

static int lua_media_player_volume_set(lua_State *L) {
#ifdef USE_MEDIA_PLAYER
  const char *name = luaL_checkstring(L, 1);
  float volume = (float) luaL_checknumber(L, 2);
  if (volume < 0.0f) volume = 0.0f;
  if (volume > 1.0f) volume = 1.0f;
  auto *player = get_media_player(L, name);
  player->make_call().set_volume(volume).perform();
  return 0;
#else
  return luaL_error(L, "media_player support is not enabled");
#endif
}

static int lua_media_player_get_volume(lua_State *L) {
#ifdef USE_MEDIA_PLAYER
  const char *name = luaL_checkstring(L, 1);
  auto *player = get_media_player(L, name);
  lua_pushnumber(L, player->volume);
  return 1;
#else
  return luaL_error(L, "media_player support is not enabled");
#endif
}

static int lua_media_player_get_state(lua_State *L) {
#ifdef USE_MEDIA_PLAYER
  const char *name = luaL_checkstring(L, 1);
  auto *player = get_media_player(L, name);
  lua_pushstring(L, media_player::media_player_state_to_string(player->state));
  return 1;
#else
  return luaL_error(L, "media_player support is not enabled");
#endif
}

static int lua_media_player_last_url(lua_State *L) {
#ifdef USE_MEDIA_PLAYER
  const char *name = luaL_checkstring(L, 1);
  auto it = g_media_last_urls.find(name);
  if (it == g_media_last_urls.end()) {
    lua_pushnil(L);
  } else {
    lua_pushstring(L, it->second.c_str());
  }
  return 1;
#else
  return luaL_error(L, "media_player support is not enabled");
#endif
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

static void register_media_player_api(lua_State *L) {
  lua_newtable(L);
  lua_pushcfunction(L, lua_media_player_play);
  lua_setfield(L, -2, "play");
  lua_pushcfunction(L, lua_media_player_pause);
  lua_setfield(L, -2, "pause");
  lua_pushcfunction(L, lua_media_player_stop);
  lua_setfield(L, -2, "stop");
  lua_pushcfunction(L, lua_media_player_volume_set);
  lua_setfield(L, -2, "volume_set");
  lua_pushcfunction(L, lua_media_player_get_volume);
  lua_setfield(L, -2, "get_volume");
  lua_pushcfunction(L, lua_media_player_get_state);
  lua_setfield(L, -2, "get_state");
  lua_pushcfunction(L, lua_media_player_last_url);
  lua_setfield(L, -2, "last_url");
  lua_setglobal(L, "media_player");
}

void register_esphome_api(lua_State *L) {
  register_device_api(L);
  register_switch_api(L);
  register_rtttl_api(L);
  register_media_player_api(L);
}

}  // namespace lua_runtime
}  // namespace esphome

#endif
