--[[
农历换算（1900-2100）

数据表 lunarInfo 每项 20 位，描述一个农历年的结构：
  0x0F       闰月是几月（0 = 无闰月）
  0x10000    闰月是大月（30 天）还是小月（29 天）
  0x8000..0x10  正月到十二月的大小月（1 = 30 天，0 = 29 天）

用法：
  local lunar = require("lunar")
  local d = lunar.fromSolar(2026, 11, 22)
  -- d.year/month/day      农历年月日（数字）
  -- d.isLeap              是否闰月
  -- d.monthCn / dayCn     "十月" / "十四"
  -- d.yearCn              "丙午年"
  -- d.zodiac              "马"
  -- d.festival            节日名（可能为 nil）

只做公历 -> 农历，公历日期用自带的累加算法，不依赖 os.time（设备上 libc 是 UTC）。
二十四节气没有收录（需要另一张 200 年的日期速查表），节日只收固定日期的农历/公历节日。
]]

local M = {}

local LUNAR_INFO = {
  0x04bd8, 0x04ae0, 0x0a570, 0x054d5, 0x0d260, 0x0d950, 0x16554, 0x056a0, 0x09ad0, 0x055d2, -- 1900-1909
  0x04ae0, 0x0a5b6, 0x0a4d0, 0x0d250, 0x1d255, 0x0b540, 0x0d6a0, 0x0ada2, 0x095b0, 0x14977, -- 1910-1919
  0x04970, 0x0a4b0, 0x0b4b5, 0x06a50, 0x06d40, 0x1ab54, 0x02b60, 0x09570, 0x052f2, 0x04970, -- 1920-1929
  0x06566, 0x0d4a0, 0x0ea50, 0x06e95, 0x05ad0, 0x02b60, 0x186e3, 0x092e0, 0x1c8d7, 0x0c950, -- 1930-1939
  0x0d4a0, 0x1d8a6, 0x0b550, 0x056a0, 0x1a5b4, 0x025d0, 0x092d0, 0x0d2b2, 0x0a950, 0x0b557, -- 1940-1949
  0x06ca0, 0x0b550, 0x15355, 0x04da0, 0x0a5b0, 0x14573, 0x052b0, 0x0a9a8, 0x0e950, 0x06aa0, -- 1950-1959
  0x0aea6, 0x0ab50, 0x04b60, 0x0aae4, 0x0a570, 0x05260, 0x0f263, 0x0d950, 0x05b57, 0x056a0, -- 1960-1969
  0x096d0, 0x04dd5, 0x04ad0, 0x0a4d0, 0x0d4d4, 0x0d250, 0x0d558, 0x0b540, 0x0b6a0, 0x195a6, -- 1970-1979
  0x095b0, 0x049b0, 0x0a974, 0x0a4b0, 0x0b27a, 0x06a50, 0x06d40, 0x0af46, 0x0ab60, 0x09570, -- 1980-1989
  0x04af5, 0x04970, 0x064b0, 0x074a3, 0x0ea50, 0x06b58, 0x05ac0, 0x0ab60, 0x096d5, 0x092e0, -- 1990-1999
  0x0c960, 0x0d954, 0x0d4a0, 0x0da50, 0x07552, 0x056a0, 0x0abb7, 0x025d0, 0x092d0, 0x0cab5, -- 2000-2009
  0x0a950, 0x0b4a0, 0x0baa4, 0x0ad50, 0x055d9, 0x04ba0, 0x0a5b0, 0x15176, 0x052b0, 0x0a930, -- 2010-2019
  0x07954, 0x06aa0, 0x0ad50, 0x05b52, 0x04b60, 0x0a6e6, 0x0a4e0, 0x0d260, 0x0ea65, 0x0d530, -- 2020-2029
  0x05aa0, 0x076a3, 0x096d0, 0x04afb, 0x04ad0, 0x0a4d0, 0x1d0b6, 0x0d250, 0x0d520, 0x0dd45, -- 2030-2039
  0x0b5a0, 0x056d0, 0x055b2, 0x049b0, 0x0a577, 0x0a4b0, 0x0aa50, 0x1b255, 0x06d20, 0x0ada0, -- 2040-2049
  0x14b63, 0x09370, 0x049f8, 0x04970, 0x064b0, 0x168a6, 0x0ea50, 0x06b20, 0x1a6c4, 0x0aae0, -- 2050-2059
  0x092e0, 0x0d2e3, 0x0c960, 0x0d557, 0x0d4a0, 0x0da50, 0x05d55, 0x056a0, 0x0a6d0, 0x055d4, -- 2060-2069
  0x052d0, 0x0a9b8, 0x0a950, 0x0b4a0, 0x0b6a6, 0x0ad50, 0x055a0, 0x0aba4, 0x0a5b0, 0x052b0, -- 2070-2079
  0x0b273, 0x06930, 0x07337, 0x06aa0, 0x0ad50, 0x14b55, 0x04b60, 0x0a570, 0x054e4, 0x0d160, -- 2080-2089
  0x0e968, 0x0d520, 0x0daa0, 0x16aa6, 0x056d0, 0x04ae0, 0x0a9d4, 0x0a4d0, 0x0d150, 0x0f252, -- 2090-2099
  0x0d520,                                                                                   -- 2100
}

local MIN_YEAR, MAX_YEAR = 1900, 2100

local MONTH_NAMES = { "正", "二", "三", "四", "五", "六", "七", "八", "九", "十", "冬", "腊" }
local DAY_ONES = { "日", "一", "二", "三", "四", "五", "六", "七", "八", "九", "十" }
local DAY_TENS = { "初", "十", "廿", "三" }
local GAN = { "甲", "乙", "丙", "丁", "戊", "己", "庚", "辛", "壬", "癸" }
local ZHI = { "子", "丑", "寅", "卯", "辰", "巳", "午", "未", "申", "酉", "戌", "亥" }
local ZODIAC = { "鼠", "牛", "虎", "兔", "龙", "蛇", "马", "羊", "猴", "鸡", "狗", "猪" }

local SOLAR_MONTH_DAYS = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 }

-- 农历固定节日（按农历月-日）
local LUNAR_FESTIVALS = {
  ["1-1"] = "春节",
  ["1-15"] = "元宵节",
  ["2-2"] = "龙抬头",
  ["5-5"] = "端午节",
  ["7-7"] = "七夕",
  ["7-15"] = "中元节",
  ["8-15"] = "中秋节",
  ["9-9"] = "重阳节",
  ["12-8"] = "腊八节",
  ["12-23"] = "小年",
}

-- 公历固定节日（按公历月-日）
local SOLAR_FESTIVALS = {
  ["1-1"] = "元旦",
  ["2-14"] = "情人节",
  ["3-8"] = "妇女节",
  ["3-12"] = "植树节",
  ["4-1"] = "愚人节",
  ["5-1"] = "劳动节",
  ["5-4"] = "青年节",
  ["6-1"] = "儿童节",
  ["7-1"] = "建党节",
  ["8-1"] = "建军节",
  ["9-10"] = "教师节",
  ["10-1"] = "国庆节",
  ["12-24"] = "平安夜",
  ["12-25"] = "圣诞节",
}

local function year_info(y)
  if y < MIN_YEAR or y > MAX_YEAR then
    return nil
  end
  return LUNAR_INFO[y - MIN_YEAR + 1]
end

function M.isLeapYear(y)
  return (y % 4 == 0 and y % 100 ~= 0) or y % 400 == 0
end

function M.solarDays(y, m)
  if m == 2 then
    return M.isLeapYear(y) and 29 or 28
  end
  return SOLAR_MONTH_DAYS[m] or 0
end

function M.leapMonth(y)
  local v = year_info(y)
  if not v then
    return 0
  end
  return v & 0xF
end

function M.leapDays(y)
  if M.leapMonth(y) == 0 then
    return 0
  end
  return (year_info(y) & 0x10000) ~= 0 and 30 or 29
end

function M.monthDays(y, m)
  if m < 1 or m > 12 then
    return 0
  end
  local v = year_info(y)
  if not v then
    return 0
  end
  return (v & (0x10000 >> m)) ~= 0 and 30 or 29
end

function M.yearDays(y)
  local v = year_info(y)
  if not v then
    return 0
  end
  local sum = 348
  local bit = 0x8000
  while bit >= 0x10 do
    if (v & bit) ~= 0 then
      sum = sum + 1
    end
    bit = bit >> 1
  end
  return sum + M.leapDays(y)
end

-- 距离 1900-01-31（农历 1900 年正月初一）的天数。
--
-- 公历部分不能在 fromSolar 里逐月/逐年累加：一屏 42 个格子，每次累加要从 1900 年数到当年，
-- 翻一次月就是一万多次迭代，真机上肉眼可见地卡。这里改成开销与年份无关的算法：
--   * 整年天数用「400 年一闰」的公式一次算出；
--   * 月内偏移查一张 12 项的累计表。
local MONTH_OFFSET = { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 }

local function days_before_year(y)
  -- 1900-01-01 到 y-01-01 的天数
  local yy = y - 1
  local leaps = yy // 4 - yy // 100 + yy // 400
  local base_leaps = 1899 // 4 - 1899 // 100 + 1899 // 400
  return (yy - 1899) * 365 + (leaps - base_leaps)
end

local function days_since_epoch_day(y, m, d)
  local n = days_before_year(y) + MONTH_OFFSET[m] + d - 1
  -- 闰年的 2 月之后要多加一天
  if m > 2 and M.isLeapYear(y) then
    n = n + 1
  end
  -- 1900-01-01 到 1900-01-31 是 30 天
  return n - 30
end

function M.dayCn(d)
  if d == 10 then
    return "初十"
  elseif d == 20 then
    return "二十"
  elseif d == 30 then
    return "三十"
  end
  local tens = DAY_TENS[(d // 10) + 1] or ""
  return tens .. (DAY_ONES[(d % 10) + 1] or "")
end

function M.monthCn(m)
  return (MONTH_NAMES[m] or "?") .. "月"
end

-- 农历年起点缓存：YEAR_START[y] = 从 1900 正月初一到 y 年正月初一的天数。
-- 首次用到时整张表一次建好（126 项，几毫秒），之后每次换算都是查表。
local YEAR_START = nil

local function build_year_starts()
  local starts = { [MIN_YEAR] = 0 }
  local acc = 0
  for y = MIN_YEAR, MAX_YEAR do
    starts[y] = acc
    acc = acc + M.yearDays(y)
  end
  YEAR_START = starts
end

-- 给定「距 1900 正月初一的天数」，返回它落在哪个农历年
local function year_cursor_for(offset)
  if YEAR_START == nil then
    build_year_starts()
  end
  -- 农历年长度 353..385 天，先按 354 估一个，再在附近校正
  local guess = MIN_YEAR + offset // 354
  if guess > MAX_YEAR then guess = MAX_YEAR end
  if guess < MIN_YEAR then guess = MIN_YEAR end
  while guess > MIN_YEAR and offset < YEAR_START[guess] do
    guess = guess - 1
  end
  while guess < MAX_YEAR and offset >= YEAR_START[guess + 1] do
    guess = guess + 1
  end
  return guess
end

local function year_start(y)
  if YEAR_START == nil then
    build_year_starts()
  end
  return YEAR_START[y]
end

-- 公历 -> 农历。超出 1900-2100 或日期非法时返回 nil。
function M.fromSolar(y, m, d)
  if not year_info(y) or m < 1 or m > 12 then
    return nil
  end
  if d < 1 or d > M.solarDays(y, m) then
    return nil
  end
  if y == MIN_YEAR and (m == 1 and d < 31) then
    return nil
  end

  local offset = days_since_epoch_day(y, m, d)
  if offset < 0 then
    return nil
  end

  -- 农历年的总天数只跟年份有关，第一次用到时算一次就缓存住。
  -- 不缓存的话每个格子都要从 1900 年数一遍（42 格 × 126 年），翻页会明显卡。
  local lYear = year_cursor_for(offset)
  offset = offset - year_start(lYear)
  if not year_info(lYear) then
    return nil
  end

  local leap = M.leapMonth(lYear)
  local isLeap = false
  local month = 1
  local i = 1
  while i < 13 and offset > 0 do
    if leap > 0 and i == leap + 1 and not isLeap then
      i = i - 1
      isLeap = true
      temp = M.leapDays(lYear)
    else
      temp = M.monthDays(lYear, i)
    end
    if isLeap and i == leap + 1 then
      isLeap = false
    end
    offset = offset - temp
    i = i + 1
  end
  if offset == 0 and leap > 0 and i == leap + 1 then
    if isLeap then
      isLeap = false
    else
      isLeap = true
      i = i - 1
    end
  end
  if offset < 0 then
    offset = offset + temp
    i = i - 1
  end
  month = i

  local day = offset + 1
  local ganzhi = ((lYear - 4) % 60) + 1
  local result = {
    year = lYear,
    month = month,
    day = day,
    isLeap = isLeap,
    monthCn = M.monthCn(month),
    dayCn = M.dayCn(day),
    yearCn = GAN[((lYear - 4) % 10) + 1] .. ZHI[((lYear - 4) % 12) + 1] .. "年",
    zodiac = ZODIAC[((lYear - 4) % 12) + 1],
    ganzhiIndex = ganzhi,
  }

  local festival = nil
  if not isLeap then
    festival = LUNAR_FESTIVALS[month .. "-" .. day]
  end
  if not festival and month == 12 and day == M.monthDays(lYear, 12) then
    festival = "除夕"
  end
  if not festival then
    festival = SOLAR_FESTIVALS[m .. "-" .. d]
  end
  result.festival = festival
  return result
end

-- 某天是周几（0 = 周日，与 os.date("*t").wday - 1 一致）。
-- 锚点：1900-01-31 是周三，正好是 days_since_epoch_day 的 0 点，所以偏移量 + 3 就是周几。
-- 不依赖 os.time / os.date，设备上 libc 是 UTC 也无所谓。
function M.weekday(y, m, d)
  local offset = days_since_epoch_day(y, m, d)
  return (offset + 3) % 7
end

return M
