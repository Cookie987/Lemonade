#pragma once

#include <string>

struct lua_State;

namespace esphome {
namespace rtttl {
class Rtttl;
}  // namespace rtttl
namespace sd_mmc_card {
class SdMmc;
}  // namespace sd_mmc_card
namespace media_player {
class MediaPlayer;
}  // namespace media_player
}  // namespace esphome

namespace esphome {
namespace lua_runtime {

void set_device_info(const std::string &name, const std::string &model, const std::string &version,
                     const std::string &platform);
void register_rtttl_player(const std::string &name, esphome::rtttl::Rtttl *player);
void register_sd_mmc_card(const std::string &name, esphome::sd_mmc_card::SdMmc *card);
void register_media_player(const std::string &name, esphome::media_player::MediaPlayer *player);
void register_esphome_api(lua_State *L);

}  // namespace lua_runtime
}  // namespace esphome
