# UV-K5 副屏：无硬件测试

用真 Lua 跑真实的 `lib/k5.lua` 与 `main.lua`（桩掉 `lvgl`/`ui`/`usb`/`sys`/`log`），
把实机抓到的帧喂进去，覆盖：协议向量(CRC16/混淆/帧)、ReplyReader 容错、
Link 会话(握手/能力探测/校准/轮询)、S 表映射与静噪判定、热插拔、
UI 文字与左右对齐（含"静噪时隐藏 RSSI 条、有信号时只画主 VFO 那一侧"）、以及用到的 lvgl API 是否真实存在。

## 先编一个主机 Lua 解释器

仓库里 `components/lua_runtime/` 自带 Lua 源码（ESP-IDF 版），主机上编要垫两个 IDF 头：

```sh
mkdir -p .esphome/luatest/shim
printf '#pragma once\n#include <stdlib.h>\n#define MALLOC_CAP_SPIRAM 0\n#define MALLOC_CAP_8BIT 0\nstatic inline void heap_caps_free(void*p){free(p);}\nstatic inline void *heap_caps_realloc(void*p,size_t n,int c){(void)c;return realloc(p,n);}\nstatic inline void *heap_caps_malloc(size_t n,int c){(void)c;return malloc(n);}\n' > .esphome/luatest/shim/esp_heap_caps.h
printf '#pragma once\n#define ESP_LOGE(t,...) do{(void)(t);}while(0)\n#define ESP_LOGW(t,...) do{(void)(t);}while(0)\n#define ESP_LOGI(t,...) do{(void)(t);}while(0)\n#define ESP_LOGD(t,...) do{(void)(t);}while(0)\n' > .esphome/luatest/shim/esp_log.h

gcc -O1 -w -DLUA_32BITS -I .esphome/luatest/shim -I components/lua_runtime \
    -o .esphome/luatest/lua.exe components/lua_runtime/lua.c \
    components/lua_runtime/{lapi,lauxlib,lbaselib,lcode,lcorolib,lctype,ldblib,ldebug,ldo,ldump,lfunc,lgc,linit,liolib,llex,lmathlib,lmem,loadlib,lobject,lopcodes,loslib,lparser,lstate,lstring,lstrlib,ltable,ltablib,ltm,lundump,lutf8lib,lvm,lzio}.c -lm
```

（`-DLUA_32BITS` 是照抄固件的定义，顺带验证 32 位整数下的位运算。）

## 跑

```sh
.esphome/luatest/lua.exe packages/lua/com.lemonade.uvk5/host_tests/test.lua
```

当前结果：`80 passed, 0 failed`。

## 注意

`lauxlib.c` 是 ESP-IDF 版(用了 `heap_caps_*` 走 PSRAM)，所以上面的 shim 是必需的；
测试里的桩代码自己也要小心 Lua 的局部变量作用域 —— 局部变量的作用域从声明之后才开始，
"先定义函数、后面才 local 变量"会让函数引用到全局 nil（这个坑在本项目里出现过三次）。
