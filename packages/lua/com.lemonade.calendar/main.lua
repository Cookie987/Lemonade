--[[
日历 com.lemonade.calendar

两个视图：月历网格 + 单日详情。

月历网格：7 列 × 6 行，格子显示公历日号；今天实底高亮，选中格描边，
有备注的格子在右上角点一个小圆点；节日（农历固定节日 + 公历固定节日）用红色日号区分。
底部两行显示选中日期的公历 + 星期 + 农历，以及备注/节日的首行。

单日详情：农历年月日、干支 + 生肖、节日，以及备注的查看与编辑。
备注用 fskv 保存（每个日期一个 key），不写应用目录，升级/卸载不会丢。

关于渲染开销（真机翻页曾经很卡，两个原因都已处理）：
  1. 每次刷新要把 42 个格子的文字/配色/圆点都写一遍。lv.* 在 Lua 侧是投递给 UI 线程的
     消息，不包 batch 就是逐条消息、逐帧刷新。这里所有整屏刷新都包在 with_batch 里，
     一次事务提交（见下面 render_month / render_day）。
  2. 农历换算。原来的实现每次从 1900 年逐年累加，一屏 42 个格子就是上万次迭代；
     现在 lib/lunar.lua 用累计表 + 公式，换算开销与年份无关（主机上快 18 倍）。
     另外这里还有一层按日期的 memo，同一屏里月历和摘要不会重复换算同一天。

关于中文输入（沿用聊天页的做法）：
  键盘用默认 map（默认 map 才有「英」键，切过去就是拼音输入），候选词列表挂到
  键盘上方的容器上。容器默认隐藏，只有键盘真的产生了候选词才显示 —— 判断方法是看
  容器里的候选按钮矩阵是否可见；没有候选时那块区域不占视觉。

关于时间：ESPHome 为了省 flash 去掉了 setenv("TZ")/tzset()，Lua 的 os.date 在设备上
拿到的是 UTC，所以优先用运行时的 rtos.localTime()（走固件配置的时区）。
]]

local sys = require("sys")
local lv = lvgl
local lunar = require("lunar")

local page = lv.app_page()
if not page then return end

lv.obj_clean(page)

-- ------------------------------------------------------------------ 常量

local PW = lv.obj_get_width(page)
local PH = lv.obj_get_height(page)
local SX, SY = PW / 320, PH / 240
local function X(v) return math.floor(v * SX) end
local function Y(v) return math.floor(v * SY) end
local function W(v) return math.floor(v * SX) end
local function H(v) return math.floor(v * SY) end

local C = {
    bg = 0xF5F7FA,
    card = 0xFFFFFF,
    text = 0x1F2937,
    dim = 0x98A2B3,
    faint = 0xB9C2CC,
    accent = 0x1B54A6,
    soft = 0xDCE8F7,
    red = 0xC0392B,
    muted_bg = 0xEEF2F7,
}

local WEEK_NAMES = { "日", "一", "二", "三", "四", "五", "六" }
local WEEK_CN = { "周日", "周一", "周二", "周三", "周四", "周五", "周六" }

local NOTES_PREFIX = "cal:n:"
local NOTE_MAX = 100

-- 版面（按 320x240 设计，运行时按实际尺寸换算）
-- 状态栏 compat 模式占 22px，所以内容从 23 开始。
local L = {
    top = 23,
    title_y = 24, title_h = 20,
    week_y = 48, week_h = 14,
    grid_y = 64, cell_h = 22, cell_w = 44,
    sum1_y = 200, sum2_y = 218,

    -- 详情页（查看态）
    dv_lunar_y = 48, dv_ganzhi_y = 66, dv_fest_y = 84,
    dv_caption_y = 106, dv_note_y = 122, dv_note_h = 64,
    dv_btn_y = 196, dv_btn_h = 28,

    -- 详情页（编辑态）：键盘占底部 96px，候选条在它上面
    kb_h = 96,
    cand_h = 28,
    ed_btn_y = 24, ed_btn_h = 20,
    ed_ta_y = 48, ed_ta_h = 64,
}

-- 候选条贴着键盘上沿摆（键盘自己用 align = BOTTOM_MID，见下面建键盘处的注释）
L.cand_y = 240 - L.kb_h - L.cand_h
L.kb_top = 240 - L.kb_h

-- ------------------------------------------------------------------ batch

-- 整屏刷新要在一次事务里提交。batch 内只会排队，batch_end 时统一刷新，
-- 否则 42 个格子 × 4 个样式 = 一百多条消息，每条都可能触发一次重绘。
local function with_batch(fn)
    lv.batch_begin()
    local ok, result = pcall(fn)
    lv.batch_end()
    if not ok then
        error(result, 0)
    end
    return result
end

-- ------------------------------------------------------------------ 日期工具

-- 日期加减。只用在一屏（±42 天）这个量级上，逐日走最不容易错；
-- 之前那个按「跳整月」的写法在跨月回退时会绕不出来（n 永远为负），别改回去。
local function add_days(y, m, d, n)
    if n == 0 then
        return y, m, d
    end
    local step = n > 0 and 1 or -1
    for _ = 1, math.abs(n) do
        d = d + step
        if d > lunar.solarDays(y, m) then
            d = 1
            m = m + 1
            if m > 12 then m = 1; y = y + 1 end
        elseif d < 1 then
            m = m - 1
            if m < 1 then m = 12; y = y - 1 end
            d = lunar.solarDays(y, m)
        end
    end
    return y, m, d
end

local function shift_month(y, m, delta)
    m = m + delta
    while m > 12 do m = m - 12; y = y + 1 end
    while m < 1 do m = m + 12; y = y - 1 end
    return y, m
end

local function current_date()
    local t
    if rtos and rtos.localTime then
        t = rtos.localTime()
    end
    if t and t.valid and t.year then
        return t.year, t.month, t.day
    end
    -- 时间还没同步（SNTP 没跑完）：os.date 在设备上是 UTC，只能兜底
    local ok, fallback = pcall(os.date, "*t")
    if ok and fallback and fallback.year then
        return fallback.year, fallback.month, fallback.day
    end
    return 2026, 1, 1
end

-- 农历换算的 memo：同一屏里月历格子、摘要、详情页会重复查同一天。
-- 只缓存最近一屏用到的（约 70 天），不做无限增长。
local lunar_cache = {}
local lunar_cache_count = 0

local function lunar_of(y, m, d)
    local key = y * 10000 + m * 100 + d
    local hit = lunar_cache[key]
    if hit ~= nil then
        return hit or nil
    end
    local ok, info = pcall(lunar.fromSolar, y, m, d)
    if not ok then info = nil end
    if lunar_cache_count > 128 then
        lunar_cache = {}
        lunar_cache_count = 0
    end
    lunar_cache[key] = info or false
    lunar_cache_count = lunar_cache_count + 1
    return info
end

-- ------------------------------------------------------------------ 备注存储

local store_ready = false

local function note_key(y, m, d)
    return string.format("%s%04d-%02d-%02d", NOTES_PREFIX, y, m, d)
end

local function get_note(y, m, d)
    if not store_ready then return "" end
    local ok, value = pcall(fskv.get, note_key(y, m, d))
    if ok and type(value) == "string" then
        return value
    end
    return ""
end

local function set_note(y, m, d, text)
    if not store_ready then return false end
    local key = note_key(y, m, d)
    local ok, result
    if text == "" then
        ok, result = pcall(fskv.del, key)
        return ok and result ~= false
    end
    ok, result = pcall(fskv.set, key, text)
    return ok and result == true
end

-- 本月有备注的日期号集合（用于格子里的小圆点）
local function month_note_days(y, m)
    local set = {}
    if not store_ready then return set end
    local ok, it = pcall(fskv.iter)
    if not ok or not it then return set end
    local prefix = string.format("%s%04d-%02d-", NOTES_PREFIX, y, m)
    while true do
        local okk, key = pcall(fskv.next, it)
        if not okk or not key then break end
        if key:sub(1, #prefix) == prefix then
            local day = tonumber(key:sub(#prefix + 1))
            if day and day >= 1 and day <= 31 then set[day] = true end
        end
    end
    return set
end

-- ------------------------------------------------------------------ 状态

local state = {
    y = 0, m = 0,
    sel_y = 0, sel_m = 0, sel_d = 0,
    today_y = 0, today_m = 0, today_d = 0,
    editing = false,   -- 编辑态：输入框 + 取消/保存在屏幕上
    kb_shown = false,  -- 键盘可见（编辑态里可以独立收起）
    cand_shown = false,
}

local refs = {}

-- ------------------------------------------------------------------ 建控件

local function mk_label(parent, x, y, w, h, text, color, align)
    local lbl = lv.label_create(parent)
    lv.obj_set_pos(lbl, x, y)
    lv.obj_set_size(lbl, w, h)
    lv.label_set_long_mode(lbl, lv.LABEL_LONG_MODE_CLIP)
    lv.obj_set_style_text_align(lbl, align or lv.TEXT_ALIGN_CENTER, lv.PART_MAIN)
    if color then
        lv.obj_set_style_text_color(lbl, color, lv.PART_MAIN)
    end
    if text then
        lv.label_set_text(lbl, text)
    end
    return lbl
end

local function mk_button(parent, x, y, w, h, text, bg, fg)
    local btn = lv.btn_create(parent)
    lv.obj_set_pos(btn, x, y)
    lv.obj_set_size(btn, w, h)
    lv.obj_set_style_bg_color(btn, bg, lv.PART_MAIN)
    lv.obj_set_style_border_width(btn, 0, lv.PART_MAIN)
    lv.obj_set_style_radius(btn, H(8), lv.PART_MAIN)
    lv.obj_set_style_shadow_width(btn, 0, lv.PART_MAIN)
    lv.obj_set_style_pad_all(btn, 0, lv.PART_MAIN)
    lv.obj_clear_flag(btn, lv.FLAG_SCROLLABLE)

    local lbl = lv.label_create(btn)
    lv.label_set_text(lbl, text)
    lv.obj_set_style_text_color(lbl, fg or 0xFFFFFF, lv.PART_MAIN)
    lv.obj_center(lbl)
    return btn, lbl
end

-- 建界面也是一大堆消息，一并包进 batch
with_batch(function()
    lv.obj_set_style_bg_color(page, C.bg, lv.PART_MAIN)
    lv.obj_set_style_border_width(page, 0, lv.PART_MAIN)
    lv.obj_clear_flag(page, lv.FLAG_SCROLLABLE)

    -- ---- 月视图
    refs.month_view = lv.obj_create(page)
    lv.obj_set_pos(refs.month_view, 0, 0)
    lv.obj_set_size(refs.month_view, PW, PH)
    lv.obj_set_style_bg_opa(refs.month_view, lv.OPA_TRANSP, lv.PART_MAIN)
    lv.obj_set_style_border_width(refs.month_view, 0, lv.PART_MAIN)
    lv.obj_set_style_pad_all(refs.month_view, 0, lv.PART_MAIN)
    lv.obj_clear_flag(refs.month_view, lv.FLAG_SCROLLABLE)

    local mv = refs.month_view

    refs.btn_prev = mk_button(mv, X(4), Y(L.title_y), W(26), H(L.title_h), "<", C.soft, C.accent)
    refs.btn_today = mk_button(mv, X(244), Y(L.title_y), W(42), H(L.title_h), "今天", C.soft, C.accent)
    refs.btn_next = mk_button(mv, X(290), Y(L.title_y), W(26), H(L.title_h), ">", C.soft, C.accent)
    refs.lbl_title = mk_label(mv, X(32), Y(L.title_y), W(208), H(L.title_h), "", C.text, lv.TEXT_ALIGN_CENTER)

    for i = 1, 7 do
        mk_label(mv, X(4 + (i - 1) * 45), Y(L.week_y), W(L.cell_w), H(L.week_h),
            WEEK_NAMES[i], (i == 1 or i == 7) and C.red or C.dim, lv.TEXT_ALIGN_CENTER)
    end

    refs.grid = lv.obj_create(mv)
    lv.obj_set_pos(refs.grid, 0, Y(L.grid_y))
    lv.obj_set_size(refs.grid, PW, H(L.cell_h * 6))
    lv.obj_set_style_bg_opa(refs.grid, lv.OPA_TRANSP, lv.PART_MAIN)
    lv.obj_set_style_border_width(refs.grid, 0, lv.PART_MAIN)
    lv.obj_set_style_pad_all(refs.grid, 0, lv.PART_MAIN)
    lv.obj_clear_flag(refs.grid, lv.FLAG_SCROLLABLE)

    refs.cells = {}
    for row = 1, 6 do
        for col = 1, 7 do
            local cell = lv.btn_create(refs.grid)
            lv.obj_set_pos(cell, X(4 + (col - 1) * 45), Y((row - 1) * L.cell_h))
            lv.obj_set_size(cell, W(L.cell_w), H(L.cell_h - 1))
            lv.obj_set_style_border_width(cell, 0, lv.PART_MAIN)
            lv.obj_set_style_radius(cell, H(6), lv.PART_MAIN)
            lv.obj_set_style_shadow_width(cell, 0, lv.PART_MAIN)
            lv.obj_set_style_pad_all(cell, 0, lv.PART_MAIN)
            lv.obj_clear_flag(cell, lv.FLAG_SCROLLABLE)

            local lbl = lv.label_create(cell)
            lv.obj_center(lbl)

            local dot = lv.obj_create(cell)
            lv.obj_set_size(dot, W(4), H(4))
            lv.obj_align(dot, lv.ALIGN_TOP_RIGHT, -W(3), H(3))
            lv.obj_set_style_radius(dot, H(2), lv.PART_MAIN)
            lv.obj_set_style_bg_color(dot, C.accent, lv.PART_MAIN)
            lv.obj_set_style_border_width(dot, 0, lv.PART_MAIN)
            lv.obj_add_flag(dot, lv.FLAG_HIDDEN)
            lv.obj_clear_flag(dot, lv.FLAG_CLICKABLE)

            refs.cells[(row - 1) * 7 + col] = { btn = cell, lbl = lbl, dot = dot }
        end
    end

    refs.lbl_sum1 = mk_label(mv, X(8), Y(L.sum1_y), W(304), H(17), "", C.text, lv.TEXT_ALIGN_LEFT)
    refs.lbl_sum2 = mk_label(mv, X(8), Y(L.sum2_y), W(304), H(17), "", C.dim, lv.TEXT_ALIGN_LEFT)

    -- ---- 日视图
    refs.day_view = lv.obj_create(page)
    lv.obj_set_pos(refs.day_view, 0, 0)
    lv.obj_set_size(refs.day_view, PW, PH)
    lv.obj_set_style_bg_color(refs.day_view, C.bg, lv.PART_MAIN)
    lv.obj_set_style_border_width(refs.day_view, 0, lv.PART_MAIN)
    lv.obj_set_style_pad_all(refs.day_view, 0, lv.PART_MAIN)
    lv.obj_clear_flag(refs.day_view, lv.FLAG_SCROLLABLE)
    lv.obj_add_flag(refs.day_view, lv.FLAG_HIDDEN)

    local dv = refs.day_view

    refs.btn_back = mk_button(dv, X(4), Y(L.title_y), W(46), H(L.title_h), "返回", C.soft, C.accent)
    refs.lbl_day_title = mk_label(dv, X(56), Y(L.title_y), W(260), H(L.title_h), "", C.text, lv.TEXT_ALIGN_RIGHT)

    refs.lbl_lunar = mk_label(dv, X(8), Y(L.dv_lunar_y), W(304), H(18), "", C.accent, lv.TEXT_ALIGN_LEFT)
    refs.lbl_ganzhi = mk_label(dv, X(8), Y(L.dv_ganzhi_y), W(304), H(18), "", C.dim, lv.TEXT_ALIGN_LEFT)
    refs.lbl_fest = mk_label(dv, X(8), Y(L.dv_fest_y), W(304), H(18), "", C.red, lv.TEXT_ALIGN_LEFT)

    refs.lbl_note_caption = mk_label(dv, X(8), Y(L.dv_caption_y), W(304), H(16), "备注", C.dim, lv.TEXT_ALIGN_LEFT)

    refs.lbl_note = lv.label_create(dv)
    lv.obj_set_pos(refs.lbl_note, X(8), Y(L.dv_note_y))
    lv.obj_set_size(refs.lbl_note, W(304), H(L.dv_note_h))
    lv.label_set_long_mode(refs.lbl_note, lv.LABEL_LONG_MODE_CLIP)
    lv.obj_set_style_text_align(refs.lbl_note, lv.TEXT_ALIGN_LEFT, lv.PART_MAIN)
    lv.obj_set_style_text_color(refs.lbl_note, C.text, lv.PART_MAIN)

    refs.btn_edit = mk_button(dv, X(8), Y(L.dv_btn_y), W(96), H(L.dv_btn_h), "编辑", C.accent, 0xFFFFFF)
    refs.btn_clear = mk_button(dv, X(112), Y(L.dv_btn_y), W(96), H(L.dv_btn_h), "删除", C.soft, C.accent)

    -- 编辑态：输入框 + 顶部两个小按钮（键盘占满下半屏，按钮只能放上面）
    refs.ta_note = lv.textarea_create(dv)
    lv.obj_set_pos(refs.ta_note, X(8), Y(L.ed_ta_y))
    lv.obj_set_size(refs.ta_note, W(304), H(L.ed_ta_h))
    lv.textarea_set_max_length(refs.ta_note, NOTE_MAX)
    lv.textarea_set_placeholder_text(refs.ta_note, "写点什么…")
    lv.obj_set_style_text_color(refs.ta_note, C.text, lv.PART_MAIN)
    lv.obj_add_flag(refs.ta_note, lv.FLAG_HIDDEN)

    refs.btn_cancel = mk_button(dv, X(4), Y(L.ed_btn_y), W(60), H(L.ed_btn_h), "取消", C.soft, C.accent)
    refs.btn_save = mk_button(dv, X(256), Y(L.ed_btn_y), W(60), H(L.ed_btn_h), "保存", C.accent, 0xFFFFFF)
    lv.obj_add_flag(refs.btn_cancel, lv.FLAG_HIDDEN)
    lv.obj_add_flag(refs.btn_save, lv.FLAG_HIDDEN)

    -- ---- 键盘 + 候选词（中文拼音输入，参照聊天页）
    --
    -- 注意 lv_keyboard_create() 的构造函数里做了 lv_obj_align(BOTTOM_MID, 0, 0)，
    -- 也就是给键盘设了 align = BOTTOM_MID、y = 0。而 LVGL 的坐标是
    -- 「align 算出来的位置 + x/y 偏移」，所以只 obj_set_pos 不改 align 的话，
    -- 键盘会被推到 240 - 96 + 144 = 288（屏幕外）。必须显式用 align 摆位。
    refs.kb = lv.keyboard_create(page)
    lv.obj_set_size(refs.kb, PW, H(L.kb_h))
    lv.obj_align(refs.kb, lv.ALIGN_BOTTOM_MID, 0, 0)
    lv.obj_add_flag(refs.kb, lv.FLAG_HIDDEN)

    -- 候选条：透明容器，候选按钮矩阵由键盘塞进来。
    -- 没有候选词时整条隐藏（sync_candidate_bar），否则空白一块白底会挡视线。
    refs.cand = lv.obj_create(page)
    lv.obj_set_pos(refs.cand, 0, Y(L.cand_y))
    lv.obj_set_size(refs.cand, PW, H(L.cand_h))
    lv.obj_set_style_bg_opa(refs.cand, lv.OPA_TRANSP, lv.PART_MAIN)
    lv.obj_set_style_border_width(refs.cand, 0, lv.PART_MAIN)
    lv.obj_set_style_pad_all(refs.cand, 0, lv.PART_MAIN)
    lv.obj_set_scroll_dir(refs.cand, lv.DIR_HOR)
    lv.obj_add_flag(refs.cand, lv.FLAG_HIDDEN)

    lv.keyboard_set_candidate_list(refs.kb, refs.cand)
    lv.keyboard_set_mode(refs.kb, lv.KEYBOARD_MODE_TEXT_LOWER)
end)

-- ------------------------------------------------------------------ 渲染

local function render_summary()
    local info = lunar_of(state.sel_y, state.sel_m, state.sel_d)
    local wday = lunar.weekday(state.sel_y, state.sel_m, state.sel_d)
    local lunar_text = ""
    if info then
        lunar_text = string.format(" · %s%s%s", info.isLeap and "闰" or "", info.monthCn, info.dayCn)
    end
    lv.label_set_text(refs.lbl_sum1, string.format("%d月%d日 %s%s",
        state.sel_m, state.sel_d, WEEK_CN[wday + 1] or "", lunar_text))

    local note = get_note(state.sel_y, state.sel_m, state.sel_d)
    if note ~= "" then
        lv.label_set_text(refs.lbl_sum2, "备注：" .. note:gsub("\n", " "))
        lv.obj_set_style_text_color(refs.lbl_sum2, C.accent, lv.PART_MAIN)
    elseif info and info.festival then
        lv.label_set_text(refs.lbl_sum2, info.festival)
        lv.obj_set_style_text_color(refs.lbl_sum2, C.red, lv.PART_MAIN)
    else
        lv.label_set_text(refs.lbl_sum2, "点击日期查看详情或添加备注")
        lv.obj_set_style_text_color(refs.lbl_sum2, C.dim, lv.PART_MAIN)
    end
end

local function render_month()
    with_batch(function()
        lv.label_set_text(refs.lbl_title, string.format("%d年%d月", state.y, state.m))

        local notes = month_note_days(state.y, state.m)
        local first_wday = lunar.weekday(state.y, state.m, 1)

        for i = 1, 42 do
            local cell = refs.cells[i]
            local cy, cm, cd = add_days(state.y, state.m, 1, (i - 1) - first_wday)

            local in_month = (cy == state.y and cm == state.m)
            local is_today = (cy == state.today_y and cm == state.today_m and cd == state.today_d)
            local is_sel = (cy == state.sel_y and cm == state.sel_m and cd == state.sel_d)
            local info = in_month and lunar_of(cy, cm, cd) or nil
            local festival = info and info.festival

            lv.label_set_text(cell.lbl, tostring(cd))

            if is_today then
                lv.obj_set_style_bg_color(cell.btn, C.accent, lv.PART_MAIN)
                lv.obj_set_style_bg_opa(cell.btn, lv.OPA_COVER, lv.PART_MAIN)
                lv.obj_set_style_text_color(cell.lbl, 0xFFFFFF, lv.PART_MAIN)
            elseif in_month then
                lv.obj_set_style_bg_color(cell.btn, festival and C.soft or C.card, lv.PART_MAIN)
                lv.obj_set_style_bg_opa(cell.btn, lv.OPA_COVER, lv.PART_MAIN)
                lv.obj_set_style_text_color(cell.lbl, festival and C.red or C.text, lv.PART_MAIN)
            else
                lv.obj_set_style_bg_color(cell.btn, C.muted_bg, lv.PART_MAIN)
                lv.obj_set_style_bg_opa(cell.btn, 80, lv.PART_MAIN)
                lv.obj_set_style_text_color(cell.lbl, C.faint, lv.PART_MAIN)
            end

            if is_sel then
                lv.obj_set_style_border_width(cell.btn, 2, lv.PART_MAIN)
                lv.obj_set_style_border_color(cell.btn, C.accent, lv.PART_MAIN)
            else
                lv.obj_set_style_border_width(cell.btn, 0, lv.PART_MAIN)
            end

            if in_month and notes[cd] then
                lv.obj_clear_flag(cell.dot, lv.FLAG_HIDDEN)
            else
                lv.obj_add_flag(cell.dot, lv.FLAG_HIDDEN)
            end
        end

        render_summary()
    end)
end

local function render_day()
    with_batch(function()
        lv.label_set_text(refs.lbl_day_title, string.format("%d月%d日 %s",
            state.sel_m, state.sel_d,
            WEEK_CN[lunar.weekday(state.sel_y, state.sel_m, state.sel_d) + 1] or ""))

        local info = lunar_of(state.sel_y, state.sel_m, state.sel_d)
        if info then
            lv.label_set_text(refs.lbl_lunar, string.format("农历%s%s%s",
                info.isLeap and "闰" or "", info.monthCn, info.dayCn))
            lv.label_set_text(refs.lbl_ganzhi, string.format("%s · 属%s", info.yearCn, info.zodiac))
            lv.label_set_text(refs.lbl_fest, info.festival or "")
        else
            lv.label_set_text(refs.lbl_lunar, "农历数据超出 1900-2100")
            lv.label_set_text(refs.lbl_ganzhi, "")
            lv.label_set_text(refs.lbl_fest, "")
        end

        local note = get_note(state.sel_y, state.sel_m, state.sel_d)
        if note == "" then
            lv.label_set_text(refs.lbl_note, "（还没有备注）")
            lv.obj_set_style_text_color(refs.lbl_note, C.dim, lv.PART_MAIN)
        else
            lv.label_set_text(refs.lbl_note, note)
            lv.obj_set_style_text_color(refs.lbl_note, C.text, lv.PART_MAIN)
        end
    end)
end

local apply_note_visibility

local function show_month_view()
    with_batch(function()
        lv.obj_add_flag(refs.day_view, lv.FLAG_HIDDEN)
        lv.obj_clear_flag(refs.month_view, lv.FLAG_HIDDEN)
    end)
    render_month()
end

local function show_day_view()
    with_batch(function()
        lv.obj_add_flag(refs.month_view, lv.FLAG_HIDDEN)
        lv.obj_clear_flag(refs.day_view, lv.FLAG_HIDDEN)
    end)
    render_day()
end

-- ------------------------------------------------------------------ 键盘 / 候选条

-- LVGL 没有"点输入框自动弹键盘"这回事，键盘显隐完全归应用管。
-- 所以这里把「编辑态」和「键盘是否可见」拆成两件事：
--   * 编辑态：输入框 + 取消/保存 在屏幕上（state.editing）
--   * 键盘：可以在编辑态里独立收起/弹出（state.kb_shown）
-- 点输入框就弹键盘；键盘自己的「收起」只收键盘，不退出编辑态，
-- 这样再点一次输入框还能弹回来。
local show_keyboard

show_keyboard = function(show)
    show = show and true or false
    if show == state.kb_shown then
        -- 已经处于目标状态，但输入框焦点可能丢过，补一次
        if show then
            lv.obj_add_state(refs.ta_note, lv.STATE_FOCUSED)
        end
        return
    end

    with_batch(function()
        if show then
            lv.keyboard_set_textarea(refs.kb, refs.ta_note)
            lv.keyboard_reset_ime(refs.kb)
            lv.obj_clear_flag(refs.kb, lv.FLAG_HIDDEN)
            lv.obj_move_to_index(refs.kb, -1)
            lv.obj_add_state(refs.ta_note, lv.STATE_FOCUSED)
        else
            lv.keyboard_reset_ime(refs.kb)
            lv.obj_add_flag(refs.kb, lv.FLAG_HIDDEN)
            lv.obj_add_flag(refs.cand, lv.FLAG_HIDDEN)
            lv.obj_clear_state(refs.ta_note, lv.STATE_FOCUSED)
        end
    end)
    state.kb_shown = show
    if show then
        -- 键盘刚弹出来时先把候选条收起来，等真有候选词再由 sync_candidate_bar 放出来
        state.cand_shown = false
        lv.obj_add_flag(refs.cand, lv.FLAG_HIDDEN)
    else
        state.cand_shown = false
    end
end

-- 键盘只在「有拼音上屏中」时才把候选按钮矩阵显出来；容器本身没有候选时不该占地方，
-- 所以跟着按钮矩阵的可见性整条隐藏（以前容器一直可见，还涂了白底，看着像多出一条 IME 区）。
local function sync_candidate_bar()
    if not state.editing or not state.kb_shown or not refs.cand then return end
    local child = lv.obj_get_child(refs.cand, 0)
    local has = child ~= nil and not lv.obj_has_flag(child, lv.FLAG_HIDDEN)
    if has == state.cand_shown then return end
    state.cand_shown = has
    if has then
        lv.obj_clear_flag(refs.cand, lv.FLAG_HIDDEN)
        lv.obj_move_to_index(refs.cand, -1)
    else
        lv.obj_add_flag(refs.cand, lv.FLAG_HIDDEN)
    end
end

-- ------------------------------------------------------------------ 编辑态

apply_note_visibility = function(editing)
    with_batch(function()
        -- 查看态的元素
        local view_items = { refs.btn_back, refs.lbl_lunar, refs.lbl_ganzhi, refs.lbl_fest,
                             refs.lbl_note_caption, refs.lbl_note, refs.btn_edit, refs.btn_clear }
        -- 编辑态的元素
        local edit_items = { refs.ta_note, refs.btn_cancel, refs.btn_save }

        for _, item in ipairs(view_items) do
            if editing then
                lv.obj_add_flag(item, lv.FLAG_HIDDEN)
            else
                lv.obj_clear_flag(item, lv.FLAG_HIDDEN)
            end
        end
        for _, item in ipairs(edit_items) do
            if editing then
                lv.obj_clear_flag(item, lv.FLAG_HIDDEN)
            else
                lv.obj_add_flag(item, lv.FLAG_HIDDEN)
            end
        end

        -- 标题在编辑态靠中间（顶上是 取消 / 保存 两个按钮）
        if editing then
            lv.obj_set_pos(refs.lbl_day_title, X(68), Y(L.ed_btn_y))
            lv.obj_set_size(refs.lbl_day_title, W(184), H(L.ed_btn_h))
            lv.obj_set_style_text_align(refs.lbl_day_title, lv.TEXT_ALIGN_CENTER, lv.PART_MAIN)
        else
            lv.obj_set_pos(refs.lbl_day_title, X(56), Y(L.title_y))
            lv.obj_set_size(refs.lbl_day_title, W(260), H(L.title_h))
            lv.obj_set_style_text_align(refs.lbl_day_title, lv.TEXT_ALIGN_RIGHT, lv.PART_MAIN)
        end
    end)
end

local function end_editing(save)
    if not state.editing then return end
    state.editing = false

    if save then
        local text = lv.textarea_get_text(refs.ta_note) or ""
        text = text:gsub("%s+$", "")
        if not set_note(state.sel_y, state.sel_m, state.sel_d, text) then
            ui.show_notification("备注保存失败", "cal", 1600)
        end
    end

    show_keyboard(false)
    apply_note_visibility(false)
    render_day()
end

local function begin_editing()
    if state.editing then
        -- 已经在编辑态（只是键盘被收起了）：再点一次就弹回来
        show_keyboard(true)
        return
    end
    state.editing = true

    local note = get_note(state.sel_y, state.sel_m, state.sel_d)
    with_batch(function()
        lv.textarea_set_text(refs.ta_note, note)
        lv.obj_set_size(refs.ta_note, W(304), H(L.ed_ta_h))
    end)
    apply_note_visibility(true)
    show_keyboard(true)
end

-- ------------------------------------------------------------------ 事件

local function select_day(y, m, d)
    state.sel_y, state.sel_m, state.sel_d = y, m, d
end

local function on_cell_clicked(index)
    local first_wday = lunar.weekday(state.y, state.m, 1)
    local cy, cm, cd = add_days(state.y, state.m, 1, index - 1 - first_wday)

    select_day(cy, cm, cd)
    -- 点到上/下月的日期时跟着翻过去，再进详情
    if cy ~= state.y or cm ~= state.m then
        state.y, state.m = cy, cm
    end
    render_month()
    show_day_view()
end

for i = 1, 42 do
    lv.obj_add_event_cb(refs.cells[i].btn, function(e)
        if lv.event_get_code(e) == lv.EVENT_CLICKED then
            on_cell_clicked(i)
        end
    end, lv.EVENT_CLICKED)
end

lv.obj_add_event_cb(refs.btn_prev, function(e)
    if lv.event_get_code(e) ~= lv.EVENT_CLICKED then return end
    state.y, state.m = shift_month(state.y, state.m, -1)
    local last = lunar.solarDays(state.y, state.m)
    if state.sel_d > last then state.sel_d = last end
    select_day(state.y, state.m, state.sel_d)
    render_month()
end, lv.EVENT_CLICKED)

lv.obj_add_event_cb(refs.btn_next, function(e)
    if lv.event_get_code(e) ~= lv.EVENT_CLICKED then return end
    state.y, state.m = shift_month(state.y, state.m, 1)
    local last = lunar.solarDays(state.y, state.m)
    if state.sel_d > last then state.sel_d = last end
    select_day(state.y, state.m, state.sel_d)
    render_month()
end, lv.EVENT_CLICKED)

lv.obj_add_event_cb(refs.btn_today, function(e)
    if lv.event_get_code(e) ~= lv.EVENT_CLICKED then return end
    state.today_y, state.today_m, state.today_d = current_date()
    state.y, state.m = state.today_y, state.today_m
    select_day(state.today_y, state.today_m, state.today_d)
    render_month()
end, lv.EVENT_CLICKED)

lv.obj_add_event_cb(refs.btn_back, function(e)
    if lv.event_get_code(e) ~= lv.EVENT_CLICKED then return end
    if state.editing then
        end_editing(false)
    end
    show_month_view()
end, lv.EVENT_CLICKED)

lv.obj_add_event_cb(refs.btn_edit, function(e)
    if lv.event_get_code(e) ~= lv.EVENT_CLICKED then return end
    begin_editing()
end, lv.EVENT_CLICKED)

lv.obj_add_event_cb(refs.btn_save, function(e)
    if lv.event_get_code(e) ~= lv.EVENT_CLICKED then return end
    end_editing(true)
end, lv.EVENT_CLICKED)

lv.obj_add_event_cb(refs.btn_cancel, function(e)
    if lv.event_get_code(e) ~= lv.EVENT_CLICKED then return end
    end_editing(false)
end, lv.EVENT_CLICKED)

lv.obj_add_event_cb(refs.btn_clear, function(e)
    if lv.event_get_code(e) ~= lv.EVENT_CLICKED then return end
    if set_note(state.sel_y, state.sel_m, state.sel_d, "") then
        ui.show_notification("备注已删除", "cal", 1200)
    end
    render_day()
end, lv.EVENT_CLICKED)

-- 键盘「收起」= 只收键盘，留在编辑态（再点输入框能弹回来）。
-- 「完成」= 保存并退出编辑态。
lv.obj_add_event_cb(refs.kb, function(e)
    if lv.event_get_code(e) == lv.EVENT_READY then
        end_editing(true)
    end
end, lv.EVENT_READY)

lv.obj_add_event_cb(refs.kb, function(e)
    if lv.event_get_code(e) == lv.EVENT_CANCEL then
        show_keyboard(false)
    end
end, lv.EVENT_CANCEL)

-- 点输入框就弹键盘（LVGL 不会自己弹）。键盘收起后输入框还留在屏幕上，
-- 这是让它重新出来唯一的入口，所以必须有。
lv.obj_add_event_cb(refs.ta_note, function(e)
    if lv.event_get_code(e) == lv.EVENT_CLICKED then
        show_keyboard(true)
    end
end, lv.EVENT_CLICKED)

lv.obj_add_event_cb(refs.ta_note, function(e)
    if lv.event_get_code(e) == lv.EVENT_FOCUSED and state.editing then
        show_keyboard(true)
    end
end, lv.EVENT_FOCUSED)

-- 回桌面时把正在编辑的备注落盘（后台约 3 秒挂起，状态会冻结）
lv.obj_add_event_cb(page, function(e)
    if lv.event_get_code(e) == lv.EVENT_SCREEN_UNLOAD_START then
        if state.editing then
            end_editing(true)
        end
    end
end, lv.EVENT_SCREEN_UNLOAD_START)

-- ------------------------------------------------------------------ 启动

local ok_init, init_result = pcall(fskv.init)
store_ready = ok_init and init_result == true

state.today_y, state.today_m, state.today_d = current_date()
state.y, state.m = state.today_y, state.today_m
select_day(state.today_y, state.today_m, state.today_d)

render_month()

if not store_ready then
    sys.timerStart(function()
        ui.show_notification("备注存储不可用，改动不会保存", "cal", 2600)
    end, 600)
end

-- 主循环：poll_events 之外，编辑中每隔几拍对一次候选条状态。
-- 键盘不对外暴露「当前有没有候选」，只能看它塞进容器的按钮矩阵是否可见。
local tick = 0
sys.timerLoopStart(function()
    lv.poll_events(10)
    tick = tick + 1
    if state.editing and state.kb_shown and (tick % 5 == 0) then
        sync_candidate_bar()
    end
end, 16)

sys.run()
