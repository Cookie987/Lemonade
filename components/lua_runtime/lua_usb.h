#pragma once

struct lua_State;

namespace esphome {
namespace usb_cdc_rx {
class USBCdcRx;
}  // namespace usb_cdc_rx
}  // namespace esphome

namespace esphome {
namespace lua_runtime {

/// Wire the Lua `usb` API to a usb_cdc_rx bridge. Called by LuaRuntime when the
/// YAML config references a usb_cdc_rx component. Creates the RX/TX stream
/// buffers and subscribes to the component's raw byte stream.
void lua_usb_attach(usb_cdc_rx::USBCdcRx *source);

/// Flush queued TX bytes into the USB channel. Must run on the main loop
/// (usb_uart's output queue is single-producer), so LuaRuntime::loop() calls it.
void lua_usb_pump_tx();

/// Deliver buffered RX bytes to a script's `usb.on_receive` callback, if one is
/// registered. Called from rtos.receive() (i.e. on the Lua task, which owns the
/// Lua state) while the script is idle.
///
/// Returns true when a batch was delivered. rtos_receive() then has to return
/// straight away: the callback typically calls sys.publish(), and sys.run() only
/// dispatches the message queue *outside* rtos.receive() - staying in the
/// receive loop would make a waiting coroutine wake up on its timeout timer
/// instead of on the message.
bool lua_usb_rtos_pump(lua_State *L);

/// Register the `usb` global table: write/read/available/flush/connected/on_receive.
void register_usb_api(lua_State *L);

}  // namespace lua_runtime
}  // namespace esphome
