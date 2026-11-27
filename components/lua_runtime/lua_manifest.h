#pragma once

#include <string>

namespace esphome {
namespace lua_runtime {

// Lua 应用 API level。
//
// 语义（升级规则见 docs/lua-app-development.md）：
//   - 新增绑定 / 新增模块：不升级（加法变更对旧应用无害）。
//   - 删除函数、修改函数签名或语义：必须 +1，并在 docs/lua-api-reference.md
//     标注「引入于 API level N」或「自 API level N 变更」。
//
// min_api 校验语义：manifest 里 min_api > LUA_API_LEVEL 的应用拒绝安装与运行，
// min_api 缺省视为 0（任意版本兼容，向后兼容没有该字段的旧包）。
constexpr int LUA_API_LEVEL = 1;

// manifest.json 的当前结构版本。
// 2 = 增加 min_api / type 字段（见 packages/lua/*/manifest.json）。
// 校验规则：manifest manifest_version 大于 MANIFEST_VERSION 支持的版本时，
// 由各消费方决定是否降级处理（桌面扫描会忽略未知结构，安装校验会拒绝）。
constexpr int LUA_MANIFEST_VERSION = 2;

// ── manifest.json 读取（无 JSON 依赖的手写解析） ──────────────────────────
//
// manifest 是 flat 对象，且字符串值不含需要转义的字符，所以用字符串查找而不是
// 引入 ArduinoJson：这个函数在 Lua 任务栈上运行（栈只有 20KB），ArduinoJson 的
// 动态分配在这里不合适。
struct LuaManifest {
  std::string name;
  std::string id;
  int min_api{0};            // 0 = 兼容任意版本
  int manifest_version{1};   // 旧包没有该字段，按 1 处理
  std::string type{"app"};   // "app"（默认）或 "lib"
};

// script_path 是 manifest 所在目录里的任意文件（通常是 .../main.lua）。
// 目录下没有 manifest.json 或解析失败时返回缺省值（name 为空）。
LuaManifest read_lua_manifest(const std::string &app_dir);

// ── min_api 校验 ────────────────────────────────────────────────────────
// 应用需要比当前固件更新的 API 时返回 false 并填充 reason（面向用户的中文提示）。
bool lua_manifest_api_supported(const LuaManifest &manifest, std::string &reason);

}  // namespace lua_runtime
}  // namespace esphome
