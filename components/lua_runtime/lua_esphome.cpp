#include "lua_esphome.h"

#ifdef LUA_RUNTIME_STUB
struct lua_State;

namespace esphome {
namespace lua_runtime {

void set_device_info(const std::string &, const std::string &, const std::string &, const std::string &) {}
void register_sd_mmc_card(const std::string &, esphome::sd_mmc_card::SdMmc *) {}
void register_media_player(const std::string &, esphome::media_player::MediaPlayer *) {}
void register_esphome_api(lua_State *) {}

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
static std::unordered_map<std::string, sd_mmc_card::SdMmc *> g_sd_mmc_cards;
static std::unordered_map<std::string, media_player::MediaPlayer *> g_media_players;
static std::unordered_map<std::string, std::string> g_media_last_urls;
static std::string g_device_name;
static std::string g_device_model;
static std::string g_device_version;
static std::string g_device_platform;

void set_device_info(const std::string &name, const std::string &model, const std::string &version,
                     const std::string &platform) {
  g_device_name = name;
  g_device_model = model;
  g_device_version = version;
  g_device_platform = platform;
}

void register_rtttl_player(const std::string &name, rtttl::Rtttl *player) {
  g_rtttl_players[name] = player;
}

void register_sd_mmc_card(const std::string &name, sd_mmc_card::SdMmc *card) {
  g_sd_mmc_cards[name] = card;
}

void register_media_player(const std::string &name, media_player::MediaPlayer *player) {
  g_media_players[name] = player;
}

static sd_mmc_card::SdMmc *get_sd_mmc_card(lua_State *L, const char *name) {
  auto it = g_sd_mmc_cards.find(name);
  if (it == g_sd_mmc_cards.end() || it->second == nullptr) {
    luaL_error(L, "sd card not found: %s", name);
    return nullptr;
  }
  return it->second;
}

static media_player::MediaPlayer *get_media_player(lua_State *L, const char *name) {
  auto it = g_media_players.find(name);
  if (it == g_media_players.end() || it->second == nullptr) {
    luaL_error(L, "media player not found: %s", name);
    return nullptr;
  }
  return it->second;
}

static float clamp_volume(float volume) {
  if (volume < 0.0f) return 0.0f;
  if (volume > 1.0f) return 1.0f;
  return volume;
}

static int lua_device_name(lua_State *L) {
  lua_pushstring(L, g_device_name.c_str());
  return 1;
}

static int lua_device_model(lua_State *L) {
  lua_pushstring(L, g_device_model.c_str());
  return 1;
}

static int lua_device_version(lua_State *L) {
  lua_pushstring(L, g_device_version.c_str());
  return 1;
}

static int lua_device_platform(lua_State *L) {
  lua_pushstring(L, g_device_platform.c_str());
  return 1;
}

static int lua_device_info(lua_State *L) {
  lua_newtable(L);
  lua_pushstring(L, g_device_name.c_str());
  lua_setfield(L, -2, "name");
  lua_pushstring(L, g_device_model.c_str());
  lua_setfield(L, -2, "model");
  lua_pushstring(L, g_device_version.c_str());
  lua_setfield(L, -2, "version");
  lua_pushstring(L, g_device_platform.c_str());
  lua_setfield(L, -2, "platform");
  return 1;
}

static int lua_sd_is_available(lua_State *L) {
#ifdef USE_SD_MMC_CARD
  const char *name = luaL_checkstring(L, 1);
  auto *card = get_sd_mmc_card(L, name);
  lua_pushboolean(L, card != nullptr && card->is_card_available());
  return 1;
#else
  return luaL_error(L, "sd_mmc_card support is not enabled");
#endif
}

static int lua_sd_is_directory(lua_State *L) {
#ifdef USE_SD_MMC_CARD
  const char *name = luaL_checkstring(L, 1);
  const char *path = luaL_checkstring(L, 2);
  auto *card = get_sd_mmc_card(L, name);
  lua_pushboolean(L, card != nullptr && card->is_directory(path));
  return 1;
#else
  return luaL_error(L, "sd_mmc_card support is not enabled");
#endif
}

static int lua_sd_list(lua_State *L) {
#ifdef USE_SD_MMC_CARD
  const char *name = luaL_checkstring(L, 1);
  const char *path = luaL_checkstring(L, 2);
  int depth = (int) luaL_optinteger(L, 3, 0);
  if (depth < 0) depth = 0;
  if (depth > 8) depth = 8;

  auto *card = get_sd_mmc_card(L, name);
  auto files = card->list_directory_file_info(path, (uint8_t) depth);
  lua_newtable(L);
  int index = 1;
  for (const auto &file : files) {
    lua_newtable(L);
    lua_pushstring(L, file.path.c_str());
    lua_setfield(L, -2, "path");
    lua_pushinteger(L, (lua_Integer) file.size);
    lua_setfield(L, -2, "size");
    lua_pushboolean(L, file.is_directory);
    lua_setfield(L, -2, "is_directory");
    lua_rawseti(L, -2, index++);
  }
  return 1;
#else
  return luaL_error(L, "sd_mmc_card support is not enabled");
#endif
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

static int lua_media_player_resume(lua_State *L) {
#ifdef USE_MEDIA_PLAYER
  return lua_media_player_command(L, media_player::MEDIA_PLAYER_COMMAND_PLAY);
#else
  return luaL_error(L, "media_player support is not enabled");
#endif
}

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
  float volume = clamp_volume((float) luaL_checknumber(L, 2));
  auto *player = get_media_player(L, name);
  player->make_call().set_volume(volume).perform();
  return 0;
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

static int lua_media_player_last_url(lua_State *L) {
  const char *name = luaL_checkstring(L, 1);
  auto it = g_media_last_urls.find(name);
  if (it == g_media_last_urls.end()) {
    lua_pushnil(L);
  } else {
    lua_pushstring(L, it->second.c_str());
  }
  return 1;
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

static void register_device_api(lua_State *L) {
  lua_newtable(L);

  lua_pushcfunction(L, lua_device_name);
  lua_setfield(L, -2, "name");
  lua_pushcfunction(L, lua_device_model);
  lua_setfield(L, -2, "model");
  lua_pushcfunction(L, lua_device_version);
  lua_setfield(L, -2, "version");
  lua_pushcfunction(L, lua_device_platform);
  lua_setfield(L, -2, "platform");
  lua_pushcfunction(L, lua_device_info);
  lua_setfield(L, -2, "info");

  lua_setglobal(L, "device");
}

static void register_sd_api(lua_State *L) {
  lua_newtable(L);

  lua_pushcfunction(L, lua_sd_is_available);
  lua_setfield(L, -2, "is_available");
  lua_pushcfunction(L, lua_sd_is_directory);
  lua_setfield(L, -2, "is_directory");
  lua_pushcfunction(L, lua_sd_list);
  lua_setfield(L, -2, "list");

  lua_setglobal(L, "sd");
}

static void register_media_player_api(lua_State *L) {
  lua_newtable(L);

  lua_pushcfunction(L, lua_media_player_play);
  lua_setfield(L, -2, "play");
  lua_pushcfunction(L, lua_media_player_resume);
  lua_setfield(L, -2, "resume");
  lua_pushcfunction(L, lua_media_player_pause);
  lua_setfield(L, -2, "pause");
  lua_pushcfunction(L, lua_media_player_stop);
  lua_setfield(L, -2, "stop");
  lua_pushcfunction(L, lua_media_player_volume_set);
  lua_setfield(L, -2, "volume_set");
  lua_pushcfunction(L, lua_media_player_get_state);
  lua_setfield(L, -2, "get_state");
  lua_pushcfunction(L, lua_media_player_get_volume);
  lua_setfield(L, -2, "get_volume");
  lua_pushcfunction(L, lua_media_player_last_url);
  lua_setfield(L, -2, "last_url");

  lua_setglobal(L, "media_player");
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
  register_device_api(L);
  register_switch_api(L);
  register_rtttl_api(L);
  register_sd_api(L);
  register_media_player_api(L);
}

}  // namespace lua_runtime
}  // namespace esphome

#endif
