#pragma once

#include "esphome/components/image/image.h"

namespace lemonade::runtime_icon {

esphome::image::Image *load_png_icon(const char *path, int width = 96, int height = 96);

}  // namespace lemonade::runtime_icon
