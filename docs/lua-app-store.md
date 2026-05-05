# Lua 应用商店与打包规范

应用商店组件位于 `components/app_store/`，负责拉取远端索引、下载 ZIP 包、解压到 SD 卡、校验 manifest，并刷新桌面应用列表。

## 目录约定

应用安装目录：

```text
/sdcard/opt/<app_id>
```

应用商店内部目录：

```text
/sdcard/var/appstore/index.json
/sdcard/tmp/appstore/packages
/sdcard/tmp/appstore/stage
/sdcard/var/appstore/backup
```

应用私有数据不放在应用目录中，避免升级/卸载时丢失：

```text
/sdcard/var/opt/<app_id>/
```

## 商店索引

远端索引必须是 JSON，顶层包含 `apps` 数组。

示例：

```json
{
  "base_url": "https://example.com/lemonade/apps/",
  "apps": [
    {
      "id": "com.lemonade.hello",
      "name": "Hello",
      "version": "1.0.0",
      "description": "Hello 示例应用",
      "author": "Lemonade",
      "platform": ["s3_t"],
      "icon_url": "hello/icon.png",
      "package_url": "hello/com.lemonade.hello-1.0.0.zip",
      "package_sha256": ""
    }
  ]
}
```

字段：

- `base_url`：可选。相对资源 URL 会基于它解析；为空时基于索引 URL。
- `apps[].id`：必填。
- `apps[].name`：可选，默认使用 `id`。
- `apps[].version`：必填。
- `apps[].description`：可选。
- `apps[].author`：可选。
- `apps[].platform`：可选数组；如果存在，必须包含当前平台 `s3_t` 才会展示。
- `apps[].icon_url`：可选，当前组件解析但桌面图标以包内 manifest 的 `icon` 为准。
- `apps[].package_url` 或 `apps[].package`：必填。
- `apps[].package_sha256` 或 `apps[].sha256`：当前会解析保存，但安装流程尚未校验 SHA256。

索引条目缺少 `id`、`version` 或包 URL 时会被跳过。

## ZIP 包结构

ZIP 可以直接包含应用文件：

```text
manifest.json
main.lua
init.lua
assets/icon.png
lib/sys.lua
```

也可以包含一层公共根目录，解压时会自动剥离：

```text
com.lemonade.hello/
  manifest.json
  main.lua
  assets/icon.png
```

ZIP 限制：

- 支持 store method `0` 和 deflate method `8`。
- 不支持加密 ZIP。
- 不允许绝对路径、空路径或包含 `..` 的路径。
- 解压时会校验文件大小和 CRC。

## 安装流程

1. 根据索引条目下载 ZIP 到 `/sdcard/tmp/appstore/packages/<app_id>.zip`。
2. 解压到 `/sdcard/tmp/appstore/stage/<app_id>`。
3. 校验 `manifest.json`：
   - 必须存在。
   - `manifest.id` 必须等于索引中的 `id`。
   - 若 manifest 和索引都提供 `version`，两者必须相等。
4. 若旧版本存在，先移动到 backup。
5. 将 stage 目录移动到 `/sdcard/opt/<app_id>`。
6. 清理临时包和旧 backup。
7. 重新扫描已安装应用并刷新桌面。

安装或卸载前，应用商店会调用 Lua Runtime 的 `abort_all()`，中止正在运行的 Lua 脚本。

## 卸载流程

卸载会删除：

```text
/sdcard/opt/<app_id>
```

不会删除：

```text
/sdcard/var/opt/<app_id>
```

因此用户数据和 `fskv` 数据默认保留。

## 发布检查清单

- `manifest.json` 的 `id` 与商店索引一致。
- `version` 与商店索引一致。
- `platform` 包含 `s3_t`。
- `icon` 指向包内存在的图标文件，推荐 96x96 PNG。
- `main.lua` 能在缺少网络时给出错误提示。
- 使用 `app.data_path()` 或 `fskv` 保存持久数据。
- ZIP 内不要包含绝对路径、`..` 或多余外层文件。

## 本地打包示例

在应用目录的父目录执行：

```powershell
Compress-Archive -Path .\com.lemonade.hello\* -DestinationPath .\com.lemonade.hello-1.0.0.zip -Force
```

如果 ZIP 包含 `com.lemonade.hello/` 这一层目录也可以，安装器会自动识别并剥离公共根目录。

