#pragma once

#include <string>

struct lua_State;

namespace esphome {
namespace rtttl {
class Rtttl;
}  // namespace rtttl
namespace media_player {
class MediaPlayer;
}  // namespace media_player
}  // namespace esphome

namespace esphome {
namespace lua_runtime {

void register_rtttl_player(const std::string &name, esphome::rtttl::Rtttl *player);
void register_media_player(const std::string &name, esphome::media_player::MediaPlayer *player);
void register_esphome_api(lua_State *L);

}  // namespace lua_runtime
}  // namespace esphome
