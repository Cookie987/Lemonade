#pragma once

#include <cstdint>
#include <unordered_map>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_timer.h"

struct lua_State;

namespace esphome {
namespace lua_runtime {

struct RtosTimer;

struct RtosContext {
  QueueHandle_t queue{nullptr};
  SemaphoreHandle_t lock{nullptr};
  std::unordered_map<int, RtosTimer *> timers;
  uint32_t autogc_period{100};
  uint32_t autogc_mid{80};
  uint32_t autogc_high{90};
  uint32_t autogc_counter{0};
};

bool rtos_init(RtosContext &ctx);
void rtos_cleanup(RtosContext &ctx);
void rtos_set_context(RtosContext *ctx);
void register_rtos_api(lua_State *L);

}  // namespace lua_runtime
}  // namespace esphome
