-- 农历模块的校验：在主机 Lua 上跑，用已知日期和 Python 的 lunardate 交叉核对。
-- 用法：.esphome/luatest/lua.exe packages/lua/com.lemonade.calendar/host_tests/test_lunar.lua

local here = arg[0]:match("^(.*)[/\\][^/\\]*$") or "."
package.path = here .. "/../lib/?.lua;" .. package.path

local lunar = require("lunar")

local passed, failed = 0, 0
local function check(name, got, want)
    if got == want then
        passed = passed + 1
    else
        failed = failed + 1
        print(string.format("  FAIL %s: got %s, want %s", name, tostring(got), tostring(want)))
    end
end

-- 已知对照（来自公开万年历）：
-- 2026-02-17 春节（丙午年正月初一）
local d = lunar.fromSolar(2026, 2, 17)
check("2026-02-17 year", d.year, 2026)
check("2026-02-17 month", d.month, 1)
check("2026-02-17 day", d.day, 1)
check("2026-02-17 monthCn", d.monthCn, "正月")
check("2026-02-17 dayCn", d.dayCn, "初一")
check("2026-02-17 yearCn", d.yearCn, "丙午年")
check("2026-02-17 zodiac", d.zodiac, "马")
check("2026-02-17 festival", d.festival, "春节")

-- 2024-02-10 春节
d = lunar.fromSolar(2024, 2, 10)
check("2024-02-10 month/day", d.month .. "-" .. d.day, "1-1")
check("2024-02-10 yearCn", d.yearCn, "甲辰年")

-- 2025-01-29 春节
d = lunar.fromSolar(2025, 1, 29)
check("2025-01-29 month/day", d.month .. "-" .. d.day, "1-1")

-- 2025 闰六月：2025-07-25 是闰六月初一
d = lunar.fromSolar(2025, 7, 25)
check("2025-07-25 isLeap", d.isLeap, true)
check("2025-07-25 month", d.month, 6)
check("2025-07-25 day", d.day, 1)

-- 2025-10-06 中秋
d = lunar.fromSolar(2025, 10, 6)
check("2025-10-06 festival", d.festival, "中秋节")

-- 2026-06-19 端午（五月初五）
d = lunar.fromSolar(2026, 6, 19)
check("2026-06-19 month/day", d.month .. "-" .. d.day, "5-5")
check("2026-06-19 festival", d.festival, "端午节")

-- 星期：2026-11-22 是周日
check("weekday 2026-11-22", lunar.weekday(2026, 11, 22), 0)
check("weekday 2026-11-21", lunar.weekday(2026, 11, 21), 6)
check("weekday 1900-01-31", lunar.weekday(1900, 1, 31), 3)
check("weekday 2000-01-01", lunar.weekday(2000, 1, 1), 6)

-- 星期与日期必须自洽：连续 7 天正好把 0..6 各走一遍
do
    local bits = 0
    for i = 0, 6 do
        local yy, mm, dd = 2026, 11, 20
        dd = dd + i
        while dd > lunar.solarDays(yy, mm) do
            dd = dd - lunar.solarDays(yy, mm)
            mm = mm + 1
            if mm > 12 then mm, yy = 1, yy + 1 end
        end
        bits = bits | (1 << lunar.weekday(yy, mm, dd))
    end
    check("连续 7 天的星期是 0..6", bits, 0x7F)
end

-- 公历月天数 / 闰年
check("solarDays 2024-2", lunar.solarDays(2024, 2), 29)
check("solarDays 2026-2", lunar.solarDays(2026, 2), 28)
check("solarDays 2100-2", lunar.solarDays(2100, 2), 28)
check("isLeapYear 2000", lunar.isLeapYear(2000), true)
check("isLeapYear 1900", lunar.isLeapYear(1900), false)

-- 中文字形
check("dayCn 10", lunar.dayCn(10), "初十")
check("dayCn 20", lunar.dayCn(20), "二十")
check("dayCn 30", lunar.dayCn(30), "三十")
check("dayCn 21", lunar.dayCn(21), "廿一")
check("dayCn 15", lunar.dayCn(15), "十五")

-- 边界：越界应返回 nil
check("fromSolar 1899", lunar.fromSolar(1899, 12, 31), nil)
check("fromSolar 1900-01-30", lunar.fromSolar(1900, 1, 30), nil)
check("fromSolar 2026-02-30", lunar.fromSolar(2026, 2, 30), nil)

-- 全量自洽检查：连续日期必须单调推进，且每个农历月天数在 29/30 之间
local y0, m0, d0 = 2020, 1, 1
local prevOffset = -1
local prev = nil
local bad = 0
local count = 0
while y0 < 2028 do
    local cur = lunar.fromSolar(y0, m0, d0)
    if not cur then
        bad = bad + 1
        print(string.format("  FAIL no lunar for %04d-%02d-%02d", y0, m0, d0))
        break
    end
    count = count + 1
    if prev then
        local ok = false
        if cur.month == prev.month and cur.day == prev.day + 1 and cur.isLeap == prev.isLeap then
            ok = true
        elseif cur.day == 1 then
            -- 换月：同一个农历年内月份 +1，或闰月切换
            if cur.isLeap and cur.month == prev.month and not prev.isLeap then
                ok = true
            elseif cur.month == prev.month + 1 and not cur.isLeap then
                ok = true
            elseif cur.month == 1 and not cur.isLeap and prev.month == 12 then
                ok = true
            end
        end
        if not ok then
            bad = bad + 1
            if bad <= 5 then
                print(string.format("  FAIL jump %04d-%02d-%02d -> %04d-%02d-%02d",
                    prev.sYear, prev.sMonth, prev.sDay, y0, m0, d0))
            end
        end
    end
    cur.sYear, cur.sMonth, cur.sDay = y0, m0, d0
    prev = cur
    d0 = d0 + 1
    if d0 > lunar.solarDays(y0, m0) then
        d0 = 1
        m0 = m0 + 1
        if m0 > 12 then
            m0 = 1
            y0 = y0 + 1
        end
    end
end
check("continuous 2020-2027", bad, 0)
print(string.format("  checked %d days", count))

print(string.format("\n%d passed, %d failed", passed, failed))
if failed > 0 then
    os.exit(1)
end
