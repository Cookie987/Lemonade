#include "background_runtime_image.h"

#include "esphome/components/display/display_buffer.h"
#include "esphome/core/application.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

#ifdef USE_ESP_IDF
#include "esp_task_wdt.h"
#endif

#include <JPEGDEC.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <vector>

namespace lemonade::background {
namespace {

static const char *const TAG = "runtime_bg";
static constexpr int kTargetWidth = 320;
static constexpr int kTargetHeight = 240;

class RuntimeBackgroundImage : public esphome::image::Image {
 public:
  RuntimeBackgroundImage()
      : Image(nullptr, 0, 0, esphome::image::IMAGE_TYPE_RGB565, esphome::image::TRANSPARENCY_OPAQUE) {}

  ~RuntimeBackgroundImage() { this->release(); }

  bool load_from_memory(const uint8_t *data, size_t size) {
    this->release();
    if (data == nullptr || size == 0) {
      ESP_LOGW(TAG, "No JPEG data provided");
      return false;
    }

    if (!this->jpeg_.openRAM(const_cast<uint8_t *>(data), size, &RuntimeBackgroundImage::draw_callback_)) {
      ESP_LOGE(TAG, "Failed to open JPEG: %d", this->jpeg_.getLastError());
      return false;
    }

    const auto jpeg_type = this->jpeg_.getJPEGType();
    if (jpeg_type == JPEG_MODE_INVALID) {
      ESP_LOGE(TAG, "Unsupported JPEG image");
      this->jpeg_.close();
      return false;
    }
    if (jpeg_type == JPEG_MODE_PROGRESSIVE) {
      ESP_LOGE(TAG, "Progressive JPEG is not supported");
      this->jpeg_.close();
      return false;
    }

    this->source_width_ = this->jpeg_.getWidth();
    this->source_height_ = this->jpeg_.getHeight();
    if (this->source_width_ <= 0 || this->source_height_ <= 0) {
      ESP_LOGE(TAG, "Invalid JPEG dimensions: %dx%d", this->source_width_, this->source_height_);
      this->jpeg_.close();
      return false;
    }

    if (!this->allocate_buffer_(kTargetWidth, kTargetHeight)) {
      ESP_LOGE(TAG, "Failed to allocate runtime background buffer");
      this->jpeg_.close();
      return false;
    }

    this->jpeg_.setUserPointer(this);
    this->jpeg_.setPixelType(RGB8888);
    if (!this->jpeg_.decode(0, 0, 0)) {
      ESP_LOGE(TAG, "JPEG decode failed: %d", this->jpeg_.getLastError());
      this->jpeg_.close();
      this->release();
      return false;
    }

    this->jpeg_.close();
    ESP_LOGI(TAG, "Loaded custom background into RAM: %dx%d -> %dx%d", this->source_width_, this->source_height_,
             this->width_, this->height_);
    return true;
  }

  void release() {
    if (this->buffer_ != nullptr) {
      this->allocator_.deallocate(this->buffer_, this->buffer_size_);
      this->buffer_ = nullptr;
      this->buffer_size_ = 0;
    }
    this->data_start_ = nullptr;
    this->width_ = 0;
    this->height_ = 0;
    this->source_width_ = 0;
    this->source_height_ = 0;
#ifdef USE_LVGL
    std::memset(&this->dsc_, 0, sizeof(this->dsc_));
#endif
  }

  bool is_loaded() const { return this->buffer_ != nullptr; }

 private:
  static int draw_callback_(JPEGDRAW *jpeg) {
    auto *self = static_cast<RuntimeBackgroundImage *>(jpeg->pUser);
    if (self == nullptr) {
      return 0;
    }

#ifdef USE_ESP_IDF
    if (esp_task_wdt_status(nullptr) == ESP_OK) {
      esphome::App.feed_wdt();
    }
#else
    esphome::App.feed_wdt();
#endif

    size_t position = 0;
    const size_t height = static_cast<size_t>(jpeg->iHeight);
    const size_t width = static_cast<size_t>(jpeg->iWidth);

    for (size_t y = 0; y < height; y++) {
      for (size_t x = 0; x < width; x++) {
        auto rg = esphome::decode_value(jpeg->pPixels[position++]);
        auto ba = esphome::decode_value(jpeg->pPixels[position++]);
        esphome::Color color(rg[1], rg[0], ba[1], ba[0]);
        self->draw_scaled_pixel_(jpeg->x + static_cast<int>(x), jpeg->y + static_cast<int>(y), color);
      }
    }

    return 1;
  }

  bool allocate_buffer_(int width, int height) {
    const size_t required_size = static_cast<size_t>(width) * static_cast<size_t>(height) * 2;
    auto *buffer = this->allocator_.allocate(required_size);
    if (buffer == nullptr) {
      return false;
    }

    std::memset(buffer, 0, required_size);
    this->buffer_ = buffer;
    this->buffer_size_ = required_size;
    this->data_start_ = this->buffer_;
    this->width_ = width;
    this->height_ = height;
    return true;
  }

  void draw_scaled_pixel_(int source_x, int source_y, const esphome::Color &color) {
    if (this->buffer_ == nullptr || this->source_width_ <= 0 || this->source_height_ <= 0) {
      return;
    }

    const int x0 = source_x * this->width_ / this->source_width_;
    const int x1 = std::min(this->width_, ((source_x + 1) * this->width_ + this->source_width_ - 1) / this->source_width_);
    const int y0 = source_y * this->height_ / this->source_height_;
    const int y1 = std::min(this->height_, ((source_y + 1) * this->height_ + this->source_height_ - 1) / this->source_height_);

    if (x0 >= this->width_ || y0 >= this->height_) {
      return;
    }

    const uint16_t rgb565 = esphome::display::ColorUtil::color_to_565(color);
    const uint8_t lo = static_cast<uint8_t>(rgb565 & 0xFF);
    const uint8_t hi = static_cast<uint8_t>((rgb565 >> 8) & 0xFF);

    for (int y = y0; y < y1; y++) {
      for (int x = x0; x < x1; x++) {
        const size_t pos = static_cast<size_t>(x + y * this->width_) * 2;
        this->buffer_[pos + 0] = lo;
        this->buffer_[pos + 1] = hi;
      }
    }
  }

  JPEGDEC jpeg_{};
  esphome::RAMAllocator<uint8_t> allocator_{esphome::RAMAllocator<uint8_t>::ALLOC_EXTERNAL};
  uint8_t *buffer_{nullptr};
  size_t buffer_size_{0};
  int source_width_{0};
  int source_height_{0};
};

RuntimeBackgroundImage g_runtime_background;

}  // namespace

bool load_custom_background_jpg_from_memory(const uint8_t *data, size_t size) {
  return g_runtime_background.load_from_memory(data, size);
}

bool load_custom_background_jpg_from_file(const char *path) {
  if (path == nullptr || path[0] == '\0') {
    ESP_LOGW(TAG, "No background path provided");
    return false;
  }

  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file.is_open()) {
    ESP_LOGW(TAG, "Failed to open background file: %s", path);
    return false;
  }

  const std::streamsize file_size = file.tellg();
  if (file_size <= 0) {
    ESP_LOGW(TAG, "Background file is empty: %s", path);
    return false;
  }

  std::vector<uint8_t> data(static_cast<size_t>(file_size));
  file.seekg(0, std::ios::beg);
  if (!file.read(reinterpret_cast<char *>(data.data()), file_size)) {
    ESP_LOGE(TAG, "Failed to read background file: %s", path);
    return false;
  }

  return load_custom_background_jpg_from_memory(data.data(), data.size());
}

void clear_custom_background_image() { g_runtime_background.release(); }

bool has_custom_background_image() { return g_runtime_background.is_loaded(); }

esphome::image::Image *get_custom_background_image() {
  return g_runtime_background.is_loaded() ? &g_runtime_background : nullptr;
}

}  // namespace lemonade::background
