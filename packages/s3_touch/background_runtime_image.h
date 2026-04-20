#pragma once

#include "esphome/components/image/image.h"

#include <cstddef>
#include <cstdint>

namespace lemonade::background {

bool load_custom_background_jpg_from_memory(const uint8_t *data, size_t size);
bool load_custom_background_jpg_from_file(const char *path);
void clear_custom_background_image();
bool has_custom_background_image();
esphome::image::Image *get_custom_background_image();

}  // namespace lemonade::background
