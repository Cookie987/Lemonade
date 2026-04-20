#include "runtime_icon_image.h"

#include "esphome/components/display/display_buffer.h"
#include "esphome/core/application.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

#include <pngle.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace lemonade::runtime_icon {
namespace {

static const char *const TAG = "runtime_icon";
static constexpr size_t kMaxCachedIcons = 32;

static bool ends_with_png(const std::string &path) {
  if (path.size() < 4) {
    return false;
  }
  const size_t off = path.size() - 4;
  return std::tolower(static_cast<unsigned char>(path[off + 0])) == '.' &&
         std::tolower(static_cast<unsigned char>(path[off + 1])) == 'p' &&
         std::tolower(static_cast<unsigned char>(path[off + 2])) == 'n' &&
         std::tolower(static_cast<unsigned char>(path[off + 3])) == 'g';
}

class RuntimeIconImage : public esphome::image::Image {
 public:
  RuntimeIconImage(int target_width, int target_height)
      : Image(nullptr, 0, 0, esphome::image::IMAGE_TYPE_RGB565, esphome::image::TRANSPARENCY_ALPHA_CHANNEL),
        target_width_(target_width),
        target_height_(target_height) {}

  ~RuntimeIconImage() { this->release(); }

  bool load_from_file(const char *path) {
    if (path == nullptr || path[0] == '\0') {
      ESP_LOGW(TAG, "No icon path provided");
      return false;
    }

    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
      ESP_LOGW(TAG, "Failed to open icon file: %s", path);
      return false;
    }

    const std::streamsize file_size = file.tellg();
    if (file_size <= 0) {
      ESP_LOGW(TAG, "Icon file is empty: %s", path);
      return false;
    }

    std::vector<uint8_t> data(static_cast<size_t>(file_size));
    file.seekg(0, std::ios::beg);
    if (!file.read(reinterpret_cast<char *>(data.data()), file_size)) {
      ESP_LOGE(TAG, "Failed to read icon file: %s", path);
      return false;
    }

    return this->load_from_memory(data.data(), data.size());
  }

  bool load_from_memory(const uint8_t *data, size_t size) {
    this->release();
    this->pixels_decoded_ = 0;
    this->source_width_ = 0;
    this->source_height_ = 0;

    auto pngle = this->allocator_.allocate(1, PNGLE_T_SIZE);
    if (pngle == nullptr) {
      ESP_LOGE(TAG, "Failed to allocate PNG decoder");
      return false;
    }

    std::memset(pngle, 0, PNGLE_T_SIZE);
    pngle_reset(pngle);
    pngle_set_user_data(pngle, this);
    pngle_set_init_callback(pngle, &RuntimeIconImage::init_callback_);
    pngle_set_draw_callback(pngle, &RuntimeIconImage::draw_callback_);

    const int fed = pngle_feed(pngle, data, size);
    if (fed < 0) {
      ESP_LOGE(TAG, "PNG decode failed: %s", pngle_error(pngle));
      pngle_reset(pngle);
      this->allocator_.deallocate(pngle, PNGLE_T_SIZE);
      this->release();
      return false;
    }

    pngle_reset(pngle);
    this->allocator_.deallocate(pngle, PNGLE_T_SIZE);

    if (this->buffer_ == nullptr) {
      ESP_LOGE(TAG, "PNG decode produced no image buffer");
      return false;
    }

    ESP_LOGD(TAG, "Loaded icon into RAM: %dx%d -> %dx%d", this->source_width_, this->source_height_, this->width_,
             this->height_);
    return true;
  }

  void release() {
    if (this->buffer_ != nullptr) {
      this->buffer_allocator_.deallocate(this->buffer_, this->buffer_size_);
      this->buffer_ = nullptr;
      this->buffer_size_ = 0;
    }
    this->data_start_ = nullptr;
    this->width_ = 0;
    this->height_ = 0;
    this->source_width_ = 0;
    this->source_height_ = 0;
    this->pixels_decoded_ = 0;
#ifdef USE_LVGL
    std::memset(&this->dsc_, 0, sizeof(this->dsc_));
#endif
  }

 private:
  static void init_callback_(pngle_t *pngle, uint32_t w, uint32_t h) {
    auto *self = static_cast<RuntimeIconImage *>(pngle_get_user_data(pngle));
    if (self == nullptr) {
      return;
    }
    self->initialize_size_(static_cast<int>(w), static_cast<int>(h));
  }

  static void draw_callback_(pngle_t *pngle, uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint8_t rgba[4]) {
    auto *self = static_cast<RuntimeIconImage *>(pngle_get_user_data(pngle));
    if (self == nullptr) {
      return;
    }

    self->draw_scaled_rect_(static_cast<int>(x), static_cast<int>(y), static_cast<int>(w), static_cast<int>(h),
                            esphome::Color(rgba[0], rgba[1], rgba[2], rgba[3]));

    self->pixels_decoded_ += w * h;
    if ((self->pixels_decoded_ % 1024U) < (w * h)) {
      esphome::App.feed_wdt();
    }
  }

  void initialize_size_(int source_width, int source_height) {
    this->source_width_ = source_width;
    this->source_height_ = source_height;
    const int out_width = this->target_width_ > 0 ? this->target_width_ : source_width;
    const int out_height = this->target_height_ > 0 ? this->target_height_ : source_height;
    this->allocate_buffer_(out_width, out_height);
  }

  bool allocate_buffer_(int width, int height) {
    if (width <= 0 || height <= 0) {
      return false;
    }

    const size_t required_size = static_cast<size_t>(width) * static_cast<size_t>(height) * 3;
    auto *buffer = this->buffer_allocator_.allocate(required_size);
    if (buffer == nullptr) {
      ESP_LOGE(TAG, "Failed to allocate icon buffer: %dx%d", width, height);
      return false;
    }

    std::memset(buffer, 0, required_size);
    this->buffer_ = buffer;
    this->buffer_size_ = required_size;
    this->width_ = width;
    this->height_ = height;
    this->data_start_ = this->buffer_;
    return true;
  }

  void draw_scaled_rect_(int x, int y, int w, int h, const esphome::Color &color) {
    if (this->buffer_ == nullptr || this->source_width_ <= 0 || this->source_height_ <= 0) {
      return;
    }

    const int x0 = x * this->width_ / this->source_width_;
    const int x1 = std::min(this->width_, ((x + w) * this->width_ + this->source_width_ - 1) / this->source_width_);
    const int y0 = y * this->height_ / this->source_height_;
    const int y1 = std::min(this->height_, ((y + h) * this->height_ + this->source_height_ - 1) / this->source_height_);
    if (x0 >= this->width_ || y0 >= this->height_) {
      return;
    }

    const uint16_t rgb565 = esphome::display::ColorUtil::color_to_565(color);
    const uint8_t lo = static_cast<uint8_t>(rgb565 & 0xFF);
    const uint8_t hi = static_cast<uint8_t>((rgb565 >> 8) & 0xFF);
    const size_t alpha_base = static_cast<size_t>(this->width_) * static_cast<size_t>(this->height_) * 2;

    for (int yy = y0; yy < y1; yy++) {
      for (int xx = x0; xx < x1; xx++) {
        const size_t pixel_index = static_cast<size_t>(xx + yy * this->width_);
        const size_t rgb_pos = pixel_index * 2;
        this->buffer_[rgb_pos + 0] = lo;
        this->buffer_[rgb_pos + 1] = hi;
        this->buffer_[alpha_base + pixel_index] = color.w;
      }
    }
  }

  const int target_width_{0};
  const int target_height_{0};
  int source_width_{0};
  int source_height_{0};
  uint32_t pixels_decoded_{0};
  uint8_t *buffer_{nullptr};
  size_t buffer_size_{0};
  esphome::RAMAllocator<uint8_t> buffer_allocator_{esphome::RAMAllocator<uint8_t>::ALLOC_EXTERNAL};
  esphome::RAMAllocator<pngle_t> allocator_;
};

struct CacheEntry {
  std::string path;
  int width;
  int height;
  std::unique_ptr<RuntimeIconImage> image;
};

static std::vector<CacheEntry> g_cache;

}  // namespace

esphome::image::Image *load_png_icon(const char *path, int width, int height) {
  if (path == nullptr || path[0] == '\0') {
    return nullptr;
  }

  const std::string key = path;
  if (!ends_with_png(key)) {
    return nullptr;
  }

  for (auto &entry : g_cache) {
    if (entry.path == key && entry.width == width && entry.height == height) {
      return entry.image.get();
    }
  }

  if (g_cache.size() >= kMaxCachedIcons) {
    ESP_LOGW(TAG, "Icon cache full, falling back to file source: %s", path);
    return nullptr;
  }

  auto image = std::make_unique<RuntimeIconImage>(width, height);
  if (!image->load_from_file(path)) {
    return nullptr;
  }

  auto *raw = image.get();
  g_cache.push_back(CacheEntry{key, width, height, std::move(image)});
  return raw;
}

}  // namespace lemonade::runtime_icon
