--[[
算出每个 bin 字体需要覆盖哪些字符，并和 assets/*.bin 里实际的 cmap 对比。

字符来源全部是机械提取的，不靠手写：
  * 协议库里的表 / 格式化函数的可能输出（k5.POWER_NAMES、MOD_NAMES、BAND_NAMES、
    fmt_mhz、fmt_adc_volt、fmt_code、describe_channel …）
  * main.lua 源码里的字符串字面量（状态字、按钮文字等）

跑法：
  .esphome/luatest/lua.exe packages/lua/com.lemonade.uvk5/host_tests/charset.lua

输出每个字体的"必须字符"清单，以及当前 bin 里缺哪些（缺的会打 MISSING）。
]]

local here = arg[0]:match("^(.*)[/\\][^/\\]*$") or "."
local app = here .. "/.."
package.path = app .. "/lib/?.lua;" .. package.path

-- sys 是运行时注入的模块，主机上桩一下
package.preload["sys"] = function() return { wait = function() end, taskInit = function() end } end
local k5 = require("k5")

------------------------------------------------------------------ 工具
local function newset() return {} end
-- ASCII 用：只取 <128 的字节，免得 UTF-8 中文的字节混进来
local function add(set, s)
    if not s then return end
    for i = 1, #s do
        local c = s:byte(i)
        if c >= 32 and c < 127 then set[c] = true end   -- 只统计真正会被画出来的可打印 ASCII
    end
end
-- 中文用：把 UTF-8 解成码点
local function add_utf8(set, s)
    if not s then return end
    local i = 1
    while i <= #s do
        local c = s:byte(i)
        local cp, len
        if c < 0x80 then cp, len = c, 1
        elseif c < 0xE0 then cp, len = c - 0xC0, 2
        elseif c < 0xF0 then cp, len = c - 0xE0, 3
        else cp, len = c - 0xF0, 4 end
        for k = 1, len - 1 do
            local b = s:byte(i + k) or 0
            cp = cp * 64 + (b - 0x80)
        end
        if cp >= 128 then set[cp] = true end
        i = i + len
    end
end
local function add_codepoint(set, cp) set[cp] = true end
local function sorted(set)
    local t = {}
    for c in pairs(set) do t[#t + 1] = c end
    table.sort(t)
    return t
end
local function render(set)
    local out = {}
    for _, c in ipairs(sorted(set)) do
        if c >= 32 and c < 127 then out[#out + 1] = string.char(c) else out[#out + 1] = string.format("U+%04X", c) end
    end
    return table.concat(out)
end

------------------------------------------------------------------ 读 bin 字体的 cmap
local function u8(s, i) return s:byte(i) end
local function u16(s, i) return (string.unpack("<I2", s, i)) end
local function u32(s, i) return (string.unpack("<I4", s, i)) end

local function font_charset(path)
    local f = io.open(path, "rb")
    if not f then return nil end
    local data = f:read("a")
    f:close()

    local set = newset()
    if data:sub(5, 8) ~= "head" then return nil, "不是 LVGL bin 字体（缺 head 标签）" end
    local cs = u32(data, 1) + 1                     -- cmap 表的起始（1-based）
    if data:sub(cs + 4, cs + 7) ~= "cmap" then return nil, "缺 cmap 标签" end
    local count = u32(data, cs + 8)
    local table_end = cs + u32(data, cs) - 1        -- cmap 长度含 8 字节标签
    local off = cs + 12
    for _ = 1, count do
        local data_off = u32(data, off)
        local range_start = u32(data, off + 4)
        local range_len = u16(data, off + 8)
        local entries = u16(data, off + 12)
        local ftype = u8(data, off + 14)
        off = off + 16

        local base = cs + data_off
        local fits = entries > 0 and (base + 2 * entries - 1) <= table_end
        if (ftype == 0 or ftype == 1) or not fits then
            -- 连续区间：range_start .. range_start+range_length-1
            -- （字体工具对"条目数 == 区间长度"的稀疏表也不写 unicode 列表，按连续理解）
            for c = range_start, range_start + range_len - 1 do add_codepoint(set, c) end
        else
            -- 稀疏：unicode 列表存的是相对 range_start 的增量
            for k = 0, entries - 1 do
                add_codepoint(set, range_start + u16(data, base + 2 * k))
            end
        end
    end
    return set
end

------------------------------------------------------------------ 需要哪些字符
-- 1) 大字频率：只有 fmt_mhz 的输出（数字、"."、无效时的 "-"）
local freq_need = newset()
add(freq_need, "0123456789.-")

-- 2) 小字（jetbrains20）
local small_need = newset()
add(small_need, "0123456789.-")                      -- fmt_mhz / fmt_adc_volt
add(small_need, " V")                                -- fmt_adc_volt 的 "%0.2f V"
add(small_need, "ONLINE WAIT OFFLINE")               -- 状态字
add(small_need, "PTT")                               -- 按钮（另外两个按钮用中文）
add(small_need, "S+")                                -- S 表：S0..S9 / 超过 S9 的 +NN
add(small_need, "CH WX VFO")                         -- 频道名
for _, n in ipairs(k5.POWER_NAMES) do add(small_need, n) end
for _, n in ipairs(k5.MOD_NAMES) do add(small_need, n) end
for _, n in ipairs(k5.BAND_NAMES) do add(small_need, n) end
-- fmt_code 的全部可能输出
add(small_need, "CTCSS 100.0Hz")                     -- 前缀 + 数字 + 单位
add(small_need, "DCS NI")                            -- DCS 正/反码后缀
-- main.lua 里的字符串字面量（状态字、按钮、错误提示等）
do
    local f = io.open(app .. "/main.lua", "r")
    if f then
        local src = f:read("a")
        f:close()
        -- 逐行、逐字面量：先去掉行尾注释，再抓 "..." 和 '...'（不跨行）
        for line in src:gmatch("[^\n]+") do
            local code = line:gsub("%-%-.*$", "")
            for s in code:gmatch('"([^"\n]*)"') do
                -- 跳过路径之类的字面量：它们只是拿去 load_font / 记日志，不会被画到标签上
                if not s:find("/", 1, true) and not s:find("%.bin") and not s:find("%.lua") then
                    add(small_need, s)
                end
            end
            for s in code:gmatch("'([^'\n]*)'") do add(small_need, s) end
        end
    end
end

-- 3) 中文（chinese18.bin）：两个按钮 + 可选的频道名
local cn_need = newset()
add_utf8(cn_need, "菜单")
add_utf8(cn_need, "通联日志")
local cn_optional = newset()
add_utf8(cn_optional, "信道气象")                      -- CH_TEXT_CJK = true 时用
add(cn_optional, "0123456789")                        -- 那时标签里还会带数字

-- 4) 电台版本号是设备给的，没法枚举，建议整段可打印 ASCII


------------------------------------------------------------------ 对比
local assets = app .. "/assets/"
local function check(name, file, need, note)
    print(string.format("\n=== %s  (%s) ===", name, file))
    print("  必须字符：" .. render(need))
    if note then print("  " .. note) end
    local set, err = font_charset(assets .. file)
    if not set then
        print("  当前 bin：读不到（" .. tostring(err) .. "）")
        return
    end
    local missing, missing_n = newset(), 0
    for c in pairs(need) do
        if not set[c] then
            missing[c] = true
            missing_n = missing_n + 1
        end
    end
    print("  当前 bin：" .. render(set))
    if missing_n == 0 then
        print("  结果：覆盖完整 ✓")
    else
        print(string.format("  结果：缺 %d 个 —— %s", missing_n, render(missing)))
    end
end

check("大字频率 72", "freq72.bin", freq_need, "来自 fmt_mhz：数字 + \".\"，无效频率的 \"--.-----\" 需要 \"-\"")
check("大字频率 64", "freq64.bin", freq_need, "同上")
check("小字 20", "jetbrains20.bin", small_need,
    "建议直接给整段可打印 ASCII 0x20-0x7E —— 版本号是电台固件给的，内容不可预知")
check("中文 18", "chinese18.bin", cn_need,
    "可选（把 main.lua 的 CH_TEXT_CJK 改成 true 时才需要）：" .. render(cn_optional))
