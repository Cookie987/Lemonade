# Lua 运行时 API 参考

本文档按当前 `components/lua_runtime/` 实现整理。除 Lua 标准库外，运行时会注入全局对象：`log`、`rtos`、`esp`、`lemonade`、`app`、`switch`、`rtttl`、`lvgl`、`ui`、`fskv`，并注册 `json`、`http` 模块。

## 全局函数

### `delay_ms(ms)`

阻塞当前 Lua 任务指定毫秒数。运行时会分片检查 OTA / abort 状态。

```lua
delay_ms(500)
```

## `log`

日志输出模块。`log("tag", ...)` 等价于 `log.info("tag", ...)`。

常量：

- `log.LOG_SILENT`
- `log.LOG_DEBUG`
- `log.LOG_INFO`
- `log.LOG_WARN`
- `log.LOG_ERROR`
- `log.LOG_FATAL`

函数：

- `log.setLevel(level)`：设置日志级别，`level` 可为数字或字符串：`"DEBUG"`、`"INFO"`、`"WARN"`、`"ERROR"` 等。
- `log.getLevel()`：返回当前日志级别数字。
- `log.style([style])`：不传参返回当前样式；传 `0..2` 修改样式。
- `log.debug(tag, ...)`
- `log.info(tag, ...)`
- `log.warn(tag, ...)`
- `log.error(tag, ...)`

示例：

```lua
log.setLevel("DEBUG")
log.info("hello", "started", app.package())
```

## `rtos`

底层消息、定时器和系统信息接口。普通应用优先使用 `sys`，直接调用 `rtos` 时要理解其消息循环。

常量：

- `rtos.INF_TIMEOUT`：无限等待。
- `rtos.MSG_TIMER`：定时器消息 ID。

函数：

- `rtos.receive([timeout_ms]) -> msg, param, exparam`：从运行时队列读取消息。超时返回 `-1`。
- `rtos.timer_start(id, timeout_ms[, repeat]) -> 1|0`：启动定时器。`repeat == 0` 为单次，非 0 为周期。
- `rtos.timer_stop(id)`：停止定时器。
- `rtos.reboot()`：当前实现会拒绝脚本重启请求，只写日志。
- `rtos.buildDate() -> string`
- `rtos.bsp() -> string`：如 `ESP32S3`。
- `rtos.version([numeric]) -> string[, 0, 32]`
- `rtos.standy([timeout_ms])`：延时等待；函数名按现有实现保留。
- `rtos.meminfo([type]) -> total, used, max_used`：`type` 可为 `"lua"`、`"psram"` 或其他默认堆。
- `rtos.firmware() -> string`
- `rtos.setPaths(path1[, path2[, path3[, path4]]])`：向 `package.path` 前追加模块路径。路径里 `%s` 会替换成 `?`。
- `rtos.nop()`
- `rtos.autoCollectMem([period[, mid[, high]]])`：配置自动 GC 策略。

## `sys`

`sys` 是 Lua 侧协程调度库，位于 `packages/lua/lib/sys.lua`。应用通常：

```lua
local sys = require("sys")
```

任务：

- `sys.taskInit(fun, ...) -> coroutine`：创建并启动协程任务。
- `sys.taskInitEx(fun, taskName, cbFun, ...) -> coroutine`：创建带定向消息队列的高级任务。
- `sys.taskDel(taskName)`：释放高级任务记录。

等待：

- `sys.wait(ms)`：只能在 `sys.taskInit` 创建的任务中调用。
- `sys.waitUntil(id[, ms]) -> ok, ...`：等待发布消息，超时返回 `false`。
- `sys.waitUntilExt(id[, ms]) -> message|false, ...`
- `sys.waitMsg(taskName[, target[, ms]]) -> msg|nil`

定时器：

- `sys.timerStart(fnc, ms, ...) -> timer_id|nil`
- `sys.timerLoopStart(fnc, ms, ...) -> timer_id|nil`
- `sys.timerStop(timer_id)` 或 `sys.timerStop(fnc, ...)`
- `sys.timerStopAll(fnc)`
- `sys.timerIsActive(timer_id)` 或 `sys.timerIsActive(fnc, ...)`

消息：

- `sys.subscribe(id, callback)`
- `sys.unsubscribe(id, callback)`
- `sys.publish(id, ...)`
- `sys.sendMsg(taskName, param1, param2, param3, param4) -> boolean`
- `sys.cleanMsg(taskName) -> boolean`

主循环：

- `sys.run()`：持续调用 `sys.safeRun()`。
- `sys.safeRun()`：分发 Lua 消息并从 `rtos.receive()` 读取底层消息。

## `esp`

- `esp.random() -> integer`
- `esp.random(upper) -> integer`：返回 `1..upper`。
- `esp.random(lower, upper) -> integer`
- `esp.mac() -> string|nil`：返回 Wi-Fi STA MAC。

## `lemonade`

- `lemonade.uid() -> string|nil`：返回设备 UID；为空时返回 `nil`。

## `app`

应用上下文。只有脚本路径位于 `/sdcard/opt/<app_id>/...` 时才会有完整上下文。

全局常量：

- `APP_PACKAGE`
- `APP_DIR`
- `APP_DATA_DIR`

函数：

- `app.package() -> string|nil`
- `app.dir() -> string|nil`
- `app.data_dir() -> string|nil`：确保数据目录存在。
- `app.data_path(relative_path) -> string|nil`：返回私有数据路径。拒绝绝对路径和 `..`。
- `app.mkdir([relative_path]) -> boolean`：创建私有数据目录或其子目录。

示例：

```lua
local path = app.data_path("cache/result.json")
if path then
    app.mkdir("cache")
end
```

## `fskv`

应用私有文件键值存储。要求脚本位于 `/sdcard/opt/<app_id>/` 下。支持 string、number、boolean、table。

- `fskv.init() -> boolean`
- `fskv.set(key, value) -> boolean`
- `fskv.sett(key, subkey, value) -> boolean`：设置表字段；`value` 省略或为 `nil` 时删除字段。
- `fskv.get(key[, subkey]) -> value|nil`
- `fskv.del(key) -> boolean`
- `fskv.clear() -> boolean`
- `fskv.iter() -> iterator|nil`
- `fskv.next(iterator) -> key|nil`
- `fskv.status() -> file_size, volume_total, key_count`

遍历示例：

```lua
local it = fskv.iter()
while it do
    local key = fskv.next(it)
    if not key then break end
    log.info("fskv", key, fskv.get(key))
end
```

限制：

- 单个值 JSON 编码后最大约 4095 字节。
- 表会通过 JSON 保存，函数、userdata、thread 不支持。

## `json`

通过 `require("json")` 使用。

- `json.encode(value[, pretty]) -> string`
- `json.stringify(value[, pretty]) -> string`
- `json.decode(text) -> value`
- `json.parse(text) -> value`
- `json.null`：JSON null 哨兵值。

## `http`

通过 `require("http")` 使用。

```lua
local status, headers, body = http.request(method, url, headers, body, opts, ca_pem, client_cert_pem, client_key_pem, client_key_password)
```

参数：

- `method`：支持 `GET`、`POST`、`PUT`、`PATCH`、`DELETE`、`HEAD`、`OPTIONS` 等。
- `url`：`http://` 或 `https://`。
- `headers`：请求头 table，可为 `nil`。
- `body`：请求体字符串，可为 `nil`。
- `opts.timeout`：毫秒。`0` 表示无限等待；默认 10 分钟。
- `opts.dst`：若设置，响应体写入文件，第三返回值为写入字节数。
- `opts.debug`：打印调试日志。
- `opts.ipv6`：使用 IPv6 地址类型。
- `opts.callback(content_length, body_length, userdata)`：下载进度回调。
- `opts.userdata`：传给进度回调。

返回：

- 成功：`status_code, response_headers, response_body_or_bytes`
- 失败：`negative_error_code, nil, nil`

`http.request_async` 当前映射到同一实现。

## `switch`

- `switch.get_state(identifier) -> boolean|nil, err`

`identifier` 可匹配 ESPHome switch 的 object id 或 name。

## `rtttl`

- `rtttl.play(player_name, song)`
- `rtttl.stop(player_name)`
- `rtttl.is_playing(player_name) -> boolean`

找不到播放器时会抛出 Lua 错误。

## `ui`

Lemonade UI 辅助接口：

- `ui.hide_topbar()`
- `ui.show_topbar()`
- `ui.show_notification(message[, suffix[, delay_ms]])`
- `ui.close_notification([suffix])`
- `ui.font14() -> font|nil`

`suffix` 用于匹配已有通知并更新，或关闭指定通知。空 suffix 关闭全部通知。

## `lvgl`

`lvgl` 是 LVGL 的 Lua 绑定。对象、字体、定时器等以 lightuserdata 表示。

基础：

- `lvgl.app_page() -> obj|nil`
- `lvgl.poll_events([timeout_ms]) -> processed_count`
- `lvgl.batch_begin()`
- `lvgl.batch_end()`

事件：

- `lvgl.obj_add_event_cb(obj, callback, code[, user_data])`
- `lvgl.event_get_code(e) -> integer`
- `lvgl.event_get_target(e) -> obj`
- `lvgl.event_get_user_data(e) -> value`

输入：

- `lvgl.indev_get_act() -> indev|nil`
- `lvgl.indev_get_gesture_dir(indev) -> integer`
- `lvgl.indev_get_point(indev) -> {x=..., y=...}|nil`

辅助：

- `lvgl.obj_center(obj)`
- `lvgl.btn_set_text(btn, text)`
- `lvgl.label_set_text_fmt(label, fmt, ...)`
- `lvgl.dropdown_get_selected_str(dropdown) -> string`
- `lvgl.img_set_src(img, src)`
- `lvgl.font_load(path, size[, cache_size]) -> font|nil`
- `lvgl.font_set_fallback(font, fallback)`
- `lvgl.font_get_fallback(font) -> font|nil`
- `lvgl.font_free(font)`
- `lvgl.timer_create(callback, period_ms[, user_data]) -> timer`
- `lvgl.timer_del(timer)`

常用对象与控件函数：

- `obj_create`、`obj_del`、`obj_clean`
- `obj_get_child`、`obj_get_child_cnt`
- `obj_set_pos`、`obj_set_x`、`obj_set_y`、`obj_set_size`、`obj_set_width`、`obj_set_height`
- `obj_align`、`obj_align_to`、`obj_set_align`
- `obj_add_flag`、`obj_clear_flag`、`obj_has_flag`
- `obj_add_state`、`obj_clear_state`、`obj_has_state`
- `obj_set_scroll_dir`、`obj_set_scrollbar_mode`、`obj_scroll_to_view`、`obj_scroll_to_y`
- 大量 `obj_set_style_*` 样式函数
- `btn_create`
- `label_create`、`label_set_text`、`label_get_text`、`label_set_long_mode`、`label_set_recolor`
- `textarea_create` 及 `textarea_*`
- `keyboard_create`、`keyboard_set_textarea`
- `dropdown_create` 及 `dropdown_*`
- `img_create` 及 `img_*`
- `list_create`、`list_add_text`、`list_add_btn`
- `msgbox_create`、`msgbox_close`
- `btnmatrix_create` 及 `btnmatrix_*`
- `bar_create` 及 `bar_*`

完整生成绑定列表见 `components/lua_runtime/lua_lvgl_gen.h` 的 `register_lvgl_gen()`。

常用常量：

- 对齐：`ALIGN_CENTER`、`ALIGN_TOP_LEFT`、`ALIGN_TOP_MID`、`ALIGN_TOP_RIGHT`、`ALIGN_BOTTOM_MID` 等。
- flag：`FLAG_HIDDEN`、`FLAG_CLICKABLE`、`FLAG_SCROLLABLE`、`FLAG_CHECKABLE` 等。
- state：`STATE_DEFAULT`、`STATE_CHECKED`、`STATE_FOCUSED`、`STATE_PRESSED`、`STATE_DISABLED`。
- part：`PART_MAIN`、`PART_SCROLLBAR`、`PART_INDICATOR`、`PART_ITEMS`、`PART_KNOB`。
- 方向：`DIR_NONE`、`DIR_LEFT`、`DIR_RIGHT`、`DIR_TOP`、`DIR_BOTTOM`、`DIR_HOR`、`DIR_VER`、`DIR_ALL`。
- flex：`FLEX_FLOW_ROW`、`FLEX_FLOW_COLUMN`、`FLEX_FLOW_ROW_WRAP`、`FLEX_ALIGN_CENTER` 等。
- 透明度：`OPA_TRANSP`、`OPA_COVER`。
- 文本：`TEXT_ALIGN_LEFT`、`TEXT_ALIGN_CENTER`、`TEXT_ALIGN_RIGHT`。
- 事件：`EVENT_CLICKED`、`EVENT_VALUE_CHANGED`、`EVENT_FOCUSED`、`EVENT_SCREEN_UNLOAD_START` 等。
