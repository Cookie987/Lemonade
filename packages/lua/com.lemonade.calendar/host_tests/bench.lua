-- 农历换算性能基准：模拟日历翻一页（42 个格子 + 摘要）
-- 用法：.esphome/luatest/lua.exe packages/lua/com.lemonade.calendar/host_tests/bench.lua

local here = arg[0]:match("^(.*)[/\\][^/\\]*$") or "."
package.path = here .. "/../lib/?.lua;" .. package.path

local lunar = require("lunar")

local function now_ms()
    -- 主机 Lua 没有 socket，用 os.clock（CPU 时间）近似
    return os.clock() * 1000
end

-- 一个月视图 = 42 次 fromSolar（跨月补齐会多算前后几天）
local function render_month(y, m)
    local n = 0
    for i = 1, 42 do
        local d = i - 3
        local yy, mm, dd = y, m, d
        if d < 1 then
            mm = m - 1
            if mm < 1 then mm, yy = 12, y - 1 end
            dd = lunar.solarDays(yy, mm) + d
        elseif d > lunar.solarDays(y, m) then
            dd = d - lunar.solarDays(y, m)
            mm = m + 1
            if mm > 12 then mm, yy = 1, y + 1 end
        end
        local info = lunar.fromSolar(yy, mm, dd)
        if info then n = n + 1 end
    end
    return n
end

-- 预热（第一次调用会建 YEAR_START 表）
render_month(2026, 11)

local ROUNDS = 200
local t0 = now_ms()
for i = 1, ROUNDS do
    local y, m = 2026, 1 + (i % 12)
    render_month(y, m)
end
local elapsed = now_ms() - t0

print(string.format("%d 个月视图（每个 42 个格子）耗时 %.1f ms", ROUNDS, elapsed))
print(string.format("平均每个视图 %.3f ms，每次换算 %.4f ms",
    elapsed / ROUNDS, elapsed / ROUNDS / 42))

-- 建表本身的开销（模拟首次打开应用）
local t1 = now_ms()
package.loaded["lunar"] = nil
package.path = here .. "/../lib/?.lua;" .. package.path
local lunar2 = require("lunar")
lunar2.fromSolar(2026, 11, 22)
local first = now_ms() - t1
print(string.format("首次换算（含建 YEAR_START 表）%.3f ms", first))
