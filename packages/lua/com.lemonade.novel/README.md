# 小说阅读器

一个给 LemonadeOS（320x240 触摸屏）使用的纯文本小说阅读器。

## 功能

- 扫描 SD 卡上的 `.txt` 文件，按每屏 7 行分页阅读。
- 只记录文本字节位置，不做章节解析；TXT 原样排版。
- 翻页、退出、应用被切走时自动保存进度。
- 下次打开同一本书时自动回到上次阅读位置。
- 书架页显示每本书的阅读百分比，并提供“继续阅读”入口。
- 只支持 UTF-8 编码；GBK / UTF-16 文件会提示转换。

## 放书位置

把 `.txt` 文件放到以下任意目录（支持子目录，最多 3 层）：

```text
/sdcard/Books/
/sdcard/Novels/
/sdcard/小说/
/sdcard/电子书/
/sdcard/Download/
/sdcard/Documents/
/sdcard/Text/
/sdcard/txt/
```

也可以直接放在 SD 卡根目录（只扫描根目录下的 `.txt`）。

应用启动后会自动扫描；新增文件后点右上角“刷新”。

## 操作

- 书架：点一本书打开；点“继续阅读”回到最近阅读的书。
- 阅读页：点屏幕左侧 1/4 上一页，右侧 3/4 下一页；也可以点底部“上一页/下一页”。
- 阅读页左上角“书架”返回书架，返回时会立即保存进度。
- 顶部右侧显示阅读百分比，底部进度条同步显示。

## 进度保存

进度保存在应用私有目录：

```text
/sdcard/var/opt/com.lemonade.novel/fskv.json
```

每本书一个键，记录当前页在文件中的字节偏移和文件大小。卸载/升级应用不会删除这个目录。

## 打包

在 `packages/lua` 目录下执行：

```powershell
Compress-Archive -Path .\com.lemonade.novel\* -DestinationPath .\com.lemonade.novel-1.0.0.zip -Force
```

ZIP 根目录包含 `manifest.json` / `main.lua` / `init.lua` / `assets/` / `lib/` 即可。

## 开发

`lib/reader_core.lua` 是不依赖 LVGL 的纯 Lua 逻辑，可以在电脑上跑测试：

```powershell
# 在应用目录下
H:\Github\Lemonade\.esphome\luatest\lua.exe host_tests\test_reader_core.lua
H:\Github\Lemonade\.esphome\luatest\lua.exe host_tests\test_sys.lua
H:\Github\Lemonade\.esphome\luatest\lua.exe host_tests\test_app_smoke.lua
```

`lib/sys.lua` 是本应用需要的最小定时器/主循环实现，只保留
`timerStart`、`timerLoopStart`、`timerStop`、`run`，不包含任务协程。
