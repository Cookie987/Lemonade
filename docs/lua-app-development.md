# Lua 应用开发指南

本文档面向运行在 LemonadeOS 上的 Lua 应用。当前运行时基于 Lua 5.4，并由固件注入 Lemonade 专用 API、LVGL UI API、HTTP/JSON 模块和简单持久化存储。

## 应用目录

已安装应用位于 SD 卡：

```text
/sdcard/opt/<app_id>/
  manifest.json
  init.lua
  main.lua
  assets/
  lib/
```

常见文件含义：

- `manifest.json`：应用元信息。桌面扫描、应用商店校验和图标加载都会读取它。
- `init.lua`：可选。应用第一次点击时优先异步执行一次，适合做预热或迁移。
- `main.lua`：应用入口。点击图标时执行，用于创建页面 UI 和进入事件循环。
- `assets/`：图片、字体等资源。LVGL 文件路径支持相对当前脚本目录。
- `lib/`：应用私有 Lua 模块。运行时会把 `<app_dir>/lib/?.lua` 加入 `package.path`。

运行时还会加入 `/sdcard/lib/?.lua`，可放共享库。仓库内示例应用把 `sys.lua` 放在应用自己的 `lib/` 下。

## Manifest

最小示例：

```json
{
  "id": "com.lemonade.hello",
  "name": "Hello",
  "version": "1.0.0",
  "description": "一个 Lemonade Lua 示例应用",
  "author": "Lemonade",
  "platform": ["s3_t"],
  "icon": "assets/icon.png"
}
```

字段说明：

- `id`：应用唯一 ID，建议反向域名，例如 `com.lemonade.calculator`。
- `name`：桌面显示名称。
- `version`：版本号。应用商店会用它判断升级。
- `description`：详情页描述，也会作为本地已安装描述缓存。
- `author`：作者。
- `platform`：数组；当前触屏平台使用 `s3_t`。桌面扫描要求匹配。
- `icon`：相对应用目录的图标路径。桌面要求存在，推荐 96x96 PNG。

## 启动流程

桌面扫描 `/sdcard/opt` 下每个子目录：

1. 读取 `manifest.json`。
2. 检查 `platform` 是否包含 `s3_t`。
3. 读取 `icon` 并创建 320x240 应用页面。
4. 将应用目录注册为 Lua 的 app page。

点击图标时：

1. 若存在 `init.lua` 且本轮尚未运行，先执行 `/sdcard/opt/<app_id>/init.lua`。
2. 执行 `/sdcard/opt/<app_id>/main.lua`。
3. 切换到应用页面，并把顶部标题设置为应用名称。

`run_file_async` 会防止同一路径重复并发运行。`main.lua` 一般应保持运行，持续轮询 LVGL 事件。

## 最小应用

```lua
local sys = require("sys")
local lv = lvgl

local page = lv.app_page()
if not page then return end

lv.obj_clean(page)
lv.obj_set_style_bg_color(page, 0xFFFFFF, 0)

local label = lv.label_create(page)
lv.label_set_text(label, "Hello Lemonade")
lv.obj_center(label)

sys.timerLoopStart(function()
    lv.poll_events(10)
end, 16)

sys.run()
```

要点：

- `lv.app_page()` 返回该应用专属页面；拿不到页面时应直接返回。
- 进入应用时通常先 `lv.obj_clean(page)`，避免旧对象残留。
- UI 回调需要 `lv.poll_events()` 分发；示例用 16ms 循环定时器。
- 结尾调用 `sys.run()` 进入调度循环。

## 资源路径

LVGL 图片和字体 API 支持相对路径：

```lua
local font = lv.font_load("assets/JetBrainsMono-Bold-7.ttf", 32)
lv.img_set_src(img, "assets/icon.png")
```

路径解析规则：

- `assets/a.png` 会解析到当前脚本目录下。
- `/sdcard/a.png` 会转换成 LVGL 文件系统路径。
- 已带驱动器前缀的路径保持原样。

字体加载：

- `.bin` 使用 LVGL binfont。
- 其他字体按 TTF 加载，需传入字号：`lv.font_load(path, size[, cache_size])`。

## 页面与事件

推荐把所有 UI 对象建在 `page` 下，并用局部表保存引用：

```lua
local refs = {}

refs.button = lv.btn_create(page)
lv.obj_set_size(refs.button, 120, 36)
lv.obj_align(refs.button, lv.ALIGN_CENTER, 0, 0)

local label = lv.label_create(refs.button)
lv.label_set_text(label, "点击")
lv.obj_center(label)

lv.obj_add_event_cb(refs.button, function(e)
    ui.show_notification("按钮被点击", "hello", 1500)
end, lv.EVENT_CLICKED)
```

事件回调中的常用函数：

- `lv.event_get_code(e)`：事件类型。
- `lv.event_get_target(e)`：触发对象。
- `lv.event_get_user_data(e)`：注册事件时传入的数据。

## 批量 UI 更新

从 Lua 任务线程调用 LVGL 时，运行时会把操作派发到 LVGL 所在线程。大量 UI 更新建议包在 batch 中：

```lua
local function with_batch(fn)
    lv.batch_begin()
    local ok, result = pcall(fn)
    lv.batch_end()
    if not ok then error(result) end
    return result
end
```

batch 内的写操作会排队，`batch_end()` 时统一刷新。需要读取返回值的 LVGL 调用会先刷新已有队列。

## 存储

应用上下文会提供私有数据目录：

```text
/sdcard/var/opt/<app_id>/data
```

可通过 `app.data_path("file.txt")` 获取应用私有路径。简单键值存储使用 `fskv`，底层文件为：

```text
/sdcard/var/opt/<app_id>/fskv.json
```

示例：

```lua
fskv.init()
fskv.set("theme", "dark")
fskv.sett("settings", "volume", 6)

local theme = fskv.get("theme")
local volume = fskv.get("settings", "volume")
```

## 网络请求

HTTP 模块用法：

```lua
local http = require("http")
local json = require("json")

local status, headers, body = http.request("GET", "https://example.com/api", nil, nil, {
    timeout = 12000,
})

if status == 200 and type(body) == "string" then
    local data = json.decode(body)
end
```

`status` 为负数时表示底层错误码；非 2xx 仍会返回 HTTP 状态码。

## 开发建议

- UI 应用优先使用 `sys.timerLoopStart(function() lv.poll_events(10) end, 16)`。
- 网络请求前后更新 UI 时，先给出“加载中”状态，再请求，避免页面看起来卡死。
- 资源尽量放在应用目录，使用相对路径。
- 长文本 label 设置宽度和 `long_mode`，避免 320x240 屏幕溢出。
- 回调里出现错误会显示“Lua 脚本错误”页面；复杂回调建议用 `pcall` 包住关键逻辑。
- 应用卸载或升级时，应用目录会被替换；持久数据应放到 `app.data_dir()` 或 `fskv`。
