// Lua bindings for the USB CDC channel (usb_uart host mode).
//
// Threading model
// ---------------
// * The Lua script runs on its own task ("lua_task"), created by LuaRuntime.
// * usb_cdc_rx (and therefore the USB host stack) lives on the ESPHome main loop.
//
// Bytes therefore cross tasks through FreeRTOS stream buffers, which are safe
// for one reader + one writer and support timeouts:
//
//   device --(usb_cdc_rx, main loop)--> g_rx --> usb.read()/usb.on_receive() (lua task)
//   lua task --usb.write()--> g_tx --> lua_usb_pump_tx() (main loop) --> device
//
// TX is deliberately marshalled through the main loop: usb_uart's output queue is
// single-producer, so the Lua task must not call write_array() itself.
//
// `usb.on_receive(fn)` is event driven, not polled from Lua: the callback is
// stored in the script's registry and invoked by lua_usb_rtos_pump(), which
// rtos.receive() calls on the Lua task while the script is idle (every <=100 ms
// slice, so worst-case delivery latency is one slice).

#include "lua_usb.h"

#include "esphome/core/defines.h"

#if defined(LUA_RUNTIME_STUB) || !defined(USE_LUA_USB)

struct lua_State;

namespace esphome {
namespace lua_runtime {

void lua_usb_attach(usb_cdc_rx::USBCdcRx *) {}
void lua_usb_pump_tx() {}
bool lua_usb_rtos_pump(lua_State *) { return false; }
void register_usb_api(lua_State *) {}

}  // namespace lua_runtime
}  // namespace esphome

#else  // !LUA_RUNTIME_STUB && USE_LUA_USB

#include <cstring>

#include "esphome/components/usb_cdc_rx/usb_cdc_rx.h"
#include "esphome/core/log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include "lua.hpp"

namespace esphome {
namespace lua_runtime {

static const char *const TAG = "lua_usb";

// Stream buffer sizes. RX has to absorb a burst between two rtos.receive() slices;
// TX only has to hold what the script queued up before the next main-loop pass.
static constexpr size_t USB_RX_BUFFER_SIZE = 2048;
static constexpr size_t USB_TX_BUFFER_SIZE = 1024;
// Largest chunk handed to Lua / usb_uart in one go (keeps stack usage predictable).
static constexpr size_t USB_IO_CHUNK = 512;
// Bytes pushed into the channel per main-loop pass. usb_uart's output pool only
// holds a handful of 64-byte chunks, so flooding it just logs "Output pool full".
static constexpr size_t USB_TX_PER_LOOP = 128;

static usb_cdc_rx::USBCdcRx *g_source = nullptr;
static StreamBufferHandle_t g_rx = nullptr;
static StreamBufferHandle_t g_tx = nullptr;

// Registry keys (lightuserdata identity, same trick lua_runtime.cpp uses).
static const char *RX_CB_KEY = "lua_usb.rx_cb";

void lua_usb_attach(usb_cdc_rx::USBCdcRx *source) {
  if (source == nullptr || g_source == source)
    return;

  if (g_rx == nullptr) {
    g_rx = xStreamBufferCreate(USB_RX_BUFFER_SIZE, 1);
  }
  if (g_tx == nullptr) {
    g_tx = xStreamBufferCreate(USB_TX_BUFFER_SIZE, 1);
  }
  if (g_rx == nullptr || g_tx == nullptr) {
    ESP_LOGE(TAG, "Failed to create USB stream buffers");
    return;
  }

  g_source = source;
  source->add_raw_listener([](const uint8_t *data, size_t len) {
    if (g_rx == nullptr)
      return;
    // Never block here: this runs on the main loop, dropping is better than stalling.
    xStreamBufferSend(g_rx, data, len, 0);
  });
  ESP_LOGI(TAG, "Lua usb API attached to USB CDC channel");
}

void lua_usb_pump_tx() {
  if (g_source == nullptr || g_tx == nullptr)
    return;

  uint8_t buffer[USB_TX_PER_LOOP];
  size_t total = 0;
  while (total < USB_TX_PER_LOOP) {
    size_t want = USB_TX_PER_LOOP - total;
    size_t got = xStreamBufferReceive(g_tx, buffer + total, want, 0);
    if (got == 0)
      break;
    total += got;
  }
  if (total > 0)
    g_source->write_bytes(buffer, total);
}

bool lua_usb_rtos_pump(lua_State *L) {
  if (L == nullptr || g_rx == nullptr)
    return false;

  lua_pushlightuserdata(L, (void *) &RX_CB_KEY);
  lua_gettable(L, LUA_REGISTRYINDEX);
  if (!lua_isfunction(L, -1)) {
    lua_pop(L, 1);
    return false;
  }

  uint8_t buffer[USB_IO_CHUNK];
  size_t got = xStreamBufferReceive(g_rx, buffer, sizeof(buffer), 0);
  if (got == 0) {
    lua_pop(L, 1);
    return false;
  }

  lua_pushlstring(L, reinterpret_cast<const char *>(buffer), got);
  if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
    const char *err = lua_tostring(L, -1);
    ESP_LOGE(TAG, "usb.on_receive callback error: %s", err ? err : "(unknown)");
    lua_pop(L, 1);
  }
  return true;
}

static int lua_usb_write(lua_State *L) {
  size_t len = 0;
  const char *data = luaL_checklstring(L, 1, &len);
  if (g_tx == nullptr || len == 0) {
    lua_pushinteger(L, 0);
    return 1;
  }
  size_t queued = xStreamBufferSend(g_tx, data, len, pdMS_TO_TICKS(20));
  lua_pushinteger(L, static_cast<lua_Integer>(queued));
  return 1;
}

static int lua_usb_read(lua_State *L) {
  lua_Integer max_len = luaL_optinteger(L, 1, 256);
  lua_Integer timeout_ms = luaL_optinteger(L, 2, 0);
  if (g_rx == nullptr) {
    lua_pushnil(L);
    return 1;
  }
  if (max_len <= 0)
    max_len = 256;
  if (max_len > static_cast<lua_Integer>(USB_IO_CHUNK))
    max_len = USB_IO_CHUNK;
  if (timeout_ms < 0)
    timeout_ms = 0;

  uint8_t buffer[USB_IO_CHUNK];
  size_t got = xStreamBufferReceive(g_rx, buffer, static_cast<size_t>(max_len), pdMS_TO_TICKS(timeout_ms));
  if (got == 0) {
    lua_pushnil(L);
    return 1;
  }
  lua_pushlstring(L, reinterpret_cast<const char *>(buffer), got);
  return 1;
}

static int lua_usb_available(lua_State *L) {
  lua_pushinteger(L, g_rx != nullptr ? static_cast<lua_Integer>(xStreamBufferBytesAvailable(g_rx)) : 0);
  return 1;
}

static int lua_usb_flush(lua_State *L) {
  (void) L;
  if (g_rx == nullptr)
    return 0;
  uint8_t scratch[64];
  int guard = 0;
  while (xStreamBufferReceive(g_rx, scratch, sizeof(scratch), 0) > 0) {
    if (++guard > 256)
      break;
  }
  return 0;
}

static int lua_usb_connected(lua_State *L) {
  lua_pushboolean(L, g_source != nullptr && g_source->device_connected());
  return 1;
}

static int lua_usb_on_receive(lua_State *L) {
  lua_pushlightuserdata(L, (void *) &RX_CB_KEY);
  if (lua_isnoneornil(L, 1)) {
    lua_pushnil(L);
  } else {
    luaL_checktype(L, 1, LUA_TFUNCTION);
    lua_pushvalue(L, 1);
  }
  lua_settable(L, LUA_REGISTRYINDEX);
  return 0;
}

void register_usb_api(lua_State *L) {
  lua_newtable(L);  // usb

  lua_pushcfunction(L, lua_usb_write);
  lua_setfield(L, -2, "write");
  lua_pushcfunction(L, lua_usb_read);
  lua_setfield(L, -2, "read");
  lua_pushcfunction(L, lua_usb_available);
  lua_setfield(L, -2, "available");
  lua_pushcfunction(L, lua_usb_flush);
  lua_setfield(L, -2, "flush");
  lua_pushcfunction(L, lua_usb_connected);
  lua_setfield(L, -2, "connected");
  lua_pushcfunction(L, lua_usb_on_receive);
  lua_setfield(L, -2, "on_receive");

  lua_setglobal(L, "usb");
}

}  // namespace lua_runtime
}  // namespace esphome

#endif  // LUA_RUNTIME_STUB || !USE_LUA_USB
