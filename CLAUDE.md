# CLAUDE.md

给在本仓库工作的 AI 助手的上下文索引。**仓库里的中文注释和 `docs/` 是权威说明**，本文件只做导航、约定和容易踩的坑；细节以源码注释为准。

## 项目是什么

LemonadeOS：ESP32-S3 智能时钟固件（ST7789V 320×240 横屏 + FT6336 触摸 + PSRAM，16MB flash），支持 Lua 应用、应用商店、OTA、天气、聊天、MQTT。配置是 ESPHome YAML + C++ lambda，UI 用 LVGL 9（`Cookie987/lvgl` 9.5.0）。

- 底层是 **fork 的 ESPHome**（`cookie987/esphome@release`，本机 checkout 在 `H:\Github\esphome`，venv 里 editable 安装，版本 2026.10.2）。改 ESPHome 核心逻辑要去那个仓库，不要往本仓库复制 ESPHome 组件。
- IoT 平台在本机 `H:\Github\Cookie_IoT`。
- 自定义 C++ 组件在 `components/`，通过顶层 `external_components: - source: components` 引入。
- Lua 5.4 运行时 + 应用商店；Lua 应用源码在 `packages/lua/`。

## 顶层目标配置

| 文件 | 用途 | 设备名 / 平台 |
| --- | --- | --- |
| `touch_public.yaml` | 公开发布版（`name_add_mac_suffix: true`，CI 发布用的就是这个） | `lemonade-t` / `s3_t` |
| `touch_canary.yaml` | T 板 Canary（DEBUG 日志） | `lemonade-tc` / `s3_t` |
| `touch3.2_canary.yaml` | T_3.2 板 Canary，发布前置校验会编译它 | `lemonade-tc32` / `s3_t_3_2` |
| `non_touch_canary.yaml` | 已弃用！的无触摸分支（`packages/s3/`），依赖 GitHub 外部组件，**不在 CI 里** | `lemonade` |
| `host-test.yaml` | 已弃用！机 SDL 模拟器（320×240），需要本机 SDL2 | `sdl` |
| `*.custom.yaml` | 个人机型配置，被 `.gitignore` 忽略（如 `zxr-main-t.custom.yaml`） | — |

`partitions_16M.csv`：双 OTA（各 ~6.8MB）+ littlefs 2MB + nvs 128KB → **app 分区很紧**，加资源前算一下空间。

## 常用命令

本机 venv 已装好 esphome（`.venv\Scripts\esphome.exe`，Python 3.12.1）。中文日志在 Windows 控制台需要 UTF-8：

```powershell
$env:PYTHONIOENCODING='UTF-8'; $env:PYTHONUTF8='1'

.venv\Scripts\esphome.exe config touch3.2_canary.yaml    # 秒级：schema/ID/包合并校验，改完先跑这个
# 下面的除非要求，否则不要主动运行
.venv\Scripts\esphome.exe compile touch3.2_canary.yaml   # 全量编译，首次几分钟，之后增量
.venv\Scripts\esphome.exe upload --device OTA touch3.2_canary.yaml
```

- **`secrets.yaml` 必须存在**（被 gitignore）。没有时先 `cp .github/ci-secrets.yaml secrets.yaml` 用占位值，否则 `esphome config` 直接报错。
- 需要的 secret 名：`wifi_ssid` `wifi_password` `ota_password` `mqtt_server` `mqtt_username` `mqtt_password` `mqtt_register_url` `app_store_index_url` `update_info_url_t_stable` `update_info_url_t_canary` `update_info_url_t32_canary`。
- VS Code 有现成任务：`.vscode/tasks.json`（Compile / OTA Upload / Compile & Upload）。

## 目录与分层（改配置前必读）

```
packages/touch-firmware.yaml   ← 触摸系列唯一的一份包清单
packages/s3_touch/             ← T 板包：hardware / lemonade / lvgl / statusbar / script / interval / usb_mode
packages/s3_touch/features/<名>/<名>.yaml   ← 功能模块聚合器（!include 自己的 lvgl/script/style 分片）
packages/s3_touch_3.2/         ← 只有 hardware.yaml，和 T 的唯一差别
packages/s3/                   ← 旧无触摸分支，只给 non_touch_canary.yaml 用，别在这上面加新功能
packages/resources.yaml        ← 全局图片 + 字体 + glyph 表
packages/lua/                  ← 内置 Lua 应用（com.lemonade.*）与公共 lib
components/                    ← 自定义 ESPHome 组件
fonts/ imgs/                   ← 字体、图标、图片源文件
docs/                          ← Lua 应用开发文档 + 设计稿
changelog/                     ← 每个版本一份，发布时被当作 release notes
```

- **新增页面/功能：只在 `packages/touch-firmware.yaml` 的 `packages:` 里加一行**，不要往各个顶层 yaml 里抄包清单。
- 功能模块按 `features/<名>/` 目录放，用 `<名>.yaml` 聚合 `<名>-lvgl.yaml` / `<名>-script.yaml` / `<名>-style.yaml`……等其他需要拆分的内容；`lvgl:` 块可以分散在多个文件里由 ESPHome 合并，页面/msgbox 就是这样分散定义的。
- 机型差异只有两处：顶层 yaml 的 `substitutions`（设备名、型号、版本、更新地址、`lemonade_app_platform`）和选哪个 `hardware.yaml`。

### 命名与结构约定

- LVGL 对象 id：`page_*` / `msgbox_*` / `lbl_*` / `btn_*` / `obj_*` / `img_*` / `roller_*` / `switch_*`；脚本 id 用动词短语（`render_setting_menu`、`statusbar_enter`）。
- substitutions 前缀：`lemonade_*`（本项目）、`c987_*`（CookieIoT 后端）、`battery_*`、`log_level`。
- 状态栏有三种模式，新页面在 `on_load` 里要显式选中一种：`statusbar_enter`（实底栏）、`statusbar_compat`（Lua 应用，透明只留标题）、`statusbar_immersive`（主屏/锁屏/OTA/高温，整条隐藏）。详见 `packages/s3_touch/statusbar.yaml` 头部。
- 启动期 `on_boot` 用优先级阶梯（850 → 600 → … → 0 → 负数），每个重模块在启动画面上打一行 `lv_label_set_text(id(loading_label), "lemonade/app/<名>")` 再 `lv_timer_handler()` 刷一帧；新功能照这个模式加，用负优先级排在主初始化之后。
- 扩展已有脚本用 `!extend`（全仓库 50 处），例如计时器扩展 `sleep_button` / `wakeup_button`，不要复制整段脚本。
- 设计令牌集中在 `features/setting/setting-style.yaml`，设置页视觉规范看 `setting-lvgl.yaml` 头部。
- YAML 2 空格缩进、连续空行最多 1 行（`.yamllint` 有配置，但 CI 没跑 yamllint，只有 `esphome config`）。注释、日志、文档用中文；提交信息是简短中文描述，版本提交是 `Bump version to X.Y.Z`。

## 自定义组件

| 目录 | 作用 |
| --- | --- |
| `lua_runtime/` | Lua 5.4 运行时（自带 Lua 源码，`lua.c`/`luac.c` 被 `FILTER_SOURCE_FILES` 排除），`lua_*.cpp` 是 `lvgl`/`sys`/`http`/`fskv`/`ui` 等 API 绑定；`lua_lvgl_gen.h` 由 `tools/gen_lvgl_bindings.py` 按 `lvgl_bindings.txt` 从 LVGL 头文件生成 |
| `app_store/` | 拉索引、下载 ZIP、解压校验、安装/卸载、刷新桌面 |
| `sd_mmc_card/` | SD 卡（4-bit SDMMC），上游 vendored |
| `usb_mode/` | 单 Type-C 口在「设备日志 ↔ USB 主机」之间运行时切换（S3 只有一个 USB PHY） |
| `usb_cdc_rx/` `websocket_client/` | USB CDC 接收、WebSocket 客户端 |
| `lemonade_bootloader/` | 把 `bootloader_components/` 拷进构建目录的自定义 bootloader |

- `lua_runtime/__init__.py` 里 `cv.require_esphome_version(2025, 7, 0)`，组件 API 跟随 fork 的 ESPHome 版本，升级 ESPHome 时要同步回归。
- 新增 Lua 侧 LVGL 绑定：在 `lvgl_bindings.txt` 加 C 函数名 → 跑 `python components/lua_runtime/tools/gen_lvgl_bindings.py`（LVGL 根目录按 `LVGL_ROOT` 环境变量 → `.esphome/build/lemonade-tc/managed_components/lvgl__lvgl` → `.piolibdeps/.../lvgl-c` → `misc/lvgl8` 的顺序查找，也可以显式 `--lvgl-root`）→ 提交重新生成的 `lua_lvgl_gen.h`。需要回调/可变参数的留在 `lua_lvgl.cpp` 手写。

## Lua 应用

- 仓库里的 `packages/lua/com.lemonade.<app>/` 就是设备上 `/sdcard/opt/<app_id>/` 的镜像：`manifest.json` + `main.lua` + `init.lua` + `assets/` + `lib/`。
- 应用私有数据在 `/sdcard/var/opt/<app_id>/`（`app.data_path()` / `fskv`），升级卸载不会丢；**别把持久数据写进应用目录**。
- `lib/sys.lua` 的现状：多数应用自带一份（从 `packages/lua/lib/sys.lua` 复制，`novel` 是精简变体），`music` / `uvk5` 没有，走运行时的共享路径 `/sdcard/lib/?.lua`。不要修改sys.lua
- 应用后台约 3 秒挂起、PSRAM 低于 1MB 才真正停止，所以状态变化要及时持久化。
- 文档：`docs/lua-app-development.md`（应用写法、uikit 声明式 UI（较为简单的ui构建可以不用）、actor 模型）、`docs/lua-api-reference.md`（API 索引）、`docs/lua-app-store.md`（索引/ZIP/安装校验规范）。

## 版本与发布

发版要同时改三处，缺一不可：

1. 顶层 yaml 的 `esphome.project.version`（三个 touch 目标）。
2. 同文件的 `lemonade_version` substitution（`1.13.0 Public` / `1.13.0 Canary`）。
3. `changelog/<版本>-CHANGELOG.md`（分「新增功能 / 改进 / 已知问题」）。**发布流程会 `mv changelog/${VERSION}-CHANGELOG.md release_notes.md`，文件不存在则发布失败。**

打 `v*` tag 触发 `.github/workflows/publish-public-firmware.yml`：先 `compile touch3.2_canary.yaml` 卡一次（保证共享包在 T_3.2 上也能编过），再 `compile touch_public.yaml`，产出 `firmware.ota.bin` 和 `LemonadeOS-<版本>-SMCK-T-Public.bin` 并附校验和。

`.github/workflows/config-check.yml`：push/PR 对 `touch_public.yaml` / `touch_canary.yaml` / `touch3.2_canary.yaml` 跑 `esphome config`（Python 3.14），不编译。

## 常见陷阱

- **中文显示不出来/变方框**：`chinese_font14` 的字表在 `fonts/glyphs.yaml`（单行大字表），其他字号（`chinese_font18`、`chinese_font36`、`clock24`、`timer60`）的 glyph 在 `packages/resources.yaml` 里手写枚举。新增文案用到的字必须补进对应字体，否则静默缺字。图标同理：Font Awesome / MDI 的码点要加到 `resources.yaml` 的 `extras` 和 `mapping`。
- 图片要写 `resize:` 到实际显示尺寸，否则一张大图吃掉几百 KB 分区空间（历史上裁天气图省过 ~1.3MB）。
- `JPEGDEC` 的传递依赖 `bb_spi_lcd` 是 Arduino-only，必须靠 `platformio_options.lib_ignore` 忽略，别删。
- LVGL 只能在主线程（lambda / LVGL 回调）里碰；Lua 侧的 `lv.*` 是投递给 UI 线程的消息，别假设立即生效。频繁更新 UI 用 `lv.batch_begin/end` 或 uikit 的 diff。
- 低功耗相关：`App.set_loop_interval()`、`switch_powermanage_lock_apb`、light sleep 会杀 USB（见 `usb_mode.yaml` 头部），动这些之前先读那段注释。
- 不要在仓库根做全量搜索/递归扫描：`anyui_s3_t_24/`（LVGL 等外部拷贝，未跟踪）、`.esphome/`（构建产物）、`.venv/`、`misc/`、`.dsh-tmp/` 会拖出十万级文件。用 `packages/`、`components/`、`docs/` 限定范围。
- 几个文件很大，整篇读会烧上下文，先 `grep` 定位：`features/message/message.yaml`（~100KB）、`features/setting/setting-lvgl.yaml`（~98KB）、`packages/s3_touch/script.yaml`（~71KB）、`setting-script.yaml`（~50KB）。
- 改共享包（`lemonade.yaml` / `statusbar.yaml` / `script.yaml` / `resources.yaml`）后，**至少要保证 `touch3.2_canary.yaml` 能 `esphome config` 通过**，因为发布流程会拿它当门槛。

## 改动自检清单

1. `esphome config <目标>.yaml` 通过（ID 引用、包合并、schema 都在这一步暴露）。
2. 动了 C++ 组件或大改配置，`esphome compile <目标>.yaml` 跑通。
3. 新增功能记得挂进 `packages/touch-firmware.yaml`，启动项记得加 `loading_label`。
4. 动字体/图片确认分区还放得下。
5. 发版补齐三处版本号 + `changelog/<版本>-CHANGELOG.md`。
