--[[
日历应用的无硬件测试：用真 Lua 跑真实的 main.lua，桩掉 lvgl / ui / fskv / rtos / sys。

覆盖：
  1. 建界面能跑通（所有用到的 lvgl API 都在真实绑定列表里）
  2. 月历格子内容正确（首格空缺数、跨月补齐、日号与节日标记）
  3. 点击日期 -> 详情页，返回 -> 月视图，翻月，今天
  4. 备注：编辑/保存/删除，落盘到 fskv 桩，格子里出现小圆点
  5. 农历信息与 lib/lunar.lua 一致

跑法见同目录 README.md。
]]

local here = arg[0]:match("^(.*)[/\\][^/\\]*$") or "."
local app = here .. "/.."
package.path = app .. "/lib/?.lua;" .. package.path

---------------------------------------------------------------- 测试框架
local passed, failed = 0, 0
local function check(name, cond, extra)
    if cond then
        passed = passed + 1
        print("  ok   " .. name)
    else
        failed = failed + 1
        print("  FAIL " .. name .. (extra ~= nil and ("  -> " .. tostring(extra)) or ""))
    end
end
local function check_eq(name, got, want)
    check(name, got == want, string.format("got %s, want %s", tostring(got), tostring(want)))
end

---------------------------------------------------------------- 桩：log
log = setmetatable({
    info = function() end,
    warn = function() end,
    error = function() end,
    debug = function() end,
}, { __call = function() end })

---------------------------------------------------------------- 桩：LVGL
-- 记录每个对象的名字（label 的文字），以及 set 过的样式，方便断言
local next_id = 0
local objects = {}
local labels = {}          -- text -> obj（后建的覆盖先建的）
local function new_obj(kind, parent, text)
    next_id = next_id + 1
    local o = {
        __id = next_id, __kind = kind, __parent = parent, text = text or "",
        hidden = false, styles = {}, flags = {}, children = {},
        w = 0, h = 0, x = 0, y = 0,
        align = nil,   -- nil = 自由坐标（等价于 TOP_LEFT + 0 偏移）
    }
    objects[next_id] = o
    if parent and parent.children then
        table.insert(parent.children, o)
    end
    return o
end

-- LVGL 的真实坐标语义：位置 = align 算出的基准点 + x/y 偏移。
-- 这点很关键：lv_keyboard_create() 内部会设 align = BOTTOM_MID，
-- 之后只 obj_set_pos 而不改 align，控件会被推出屏幕。
local SCREEN_W, SCREEN_H = 320, 240

local function abs_pos(o)
    -- 屏幕（app page 本身）就是原点
    if o.__kind == "page" then
        return 0, 0
    end

    local px, py = 0, 0
    if o.__parent then
        px, py = abs_pos(o.__parent)
    end

    -- 父控件尺寸：没有显式尺寸就是整个屏幕
    local pw = (o.__parent and o.__parent.w ~= 0) and o.__parent.w or SCREEN_W
    local ph = (o.__parent and o.__parent.h ~= 0) and o.__parent.h or SCREEN_H

    local bx, by = 0, 0
    if o.align == 2 then                -- TOP_MID
        bx = (pw - o.w) // 2
    elseif o.align == 3 then            -- TOP_RIGHT
        bx = pw - o.w
    elseif o.align == 7 then            -- BOTTOM_MID
        bx = (pw - o.w) // 2
        by = ph - o.h
    elseif o.align == 8 then            -- BOTTOM_RIGHT
        bx = pw - o.w
        by = ph - o.h
    elseif o.align == 9 then            -- CENTER
        bx = (pw - o.w) // 2
        by = (ph - o.h) // 2
    elseif o.align == 6 then            -- LEFT_MID (近似)
        by = (ph - o.h) // 2
    end
    return px + bx + o.x, py + by + o.y
end

local lvgl_calls = {}
local CONST = {
    TEXT_ALIGN_LEFT = 0, TEXT_ALIGN_CENTER = 1, TEXT_ALIGN_RIGHT = 2,
    PART_MAIN = 0, OPA_TRANSP = 0, OPA_COVER = 255,
    FLAG_SCROLLABLE = 0x10, FLAG_CLICKABLE = 0x20, FLAG_HIDDEN = 0x1,
    LABEL_LONG_MODE_WRAP = 0, LABEL_LONG_MODE_DOTS = 1, LABEL_LONG_MODE_CLIP = 4,
    ALIGN_CENTER = 9, ALIGN_TOP_MID = 2, ALIGN_TOP_RIGHT = 3, ALIGN_BOTTOM_MID = 7,
    ALIGN_TOP_LEFT = 0, DIR_HOR = 2, DIR_ALL = 15,
    STATE_FOCUSED = 0x2, STATE_PRESSED = 0x1,
    EVENT_CLICKED = 7, EVENT_READY = 22, EVENT_CANCEL = 23,
    EVENT_VALUE_CHANGED = 27, EVENT_SCREEN_UNLOAD_START = 4,
    KEYBOARD_MODE_TEXT_LOWER = 0, KEYBOARD_MODE_IME_CHN = 8,
}

local cached = {}
local raw_lvgl = setmetatable(cached, {
    __index = function(_, key)
        local c = CONST[key]
        if c ~= nil then cached[key] = c; return c end
        local f = function(...)
            lvgl_calls[#lvgl_calls + 1] = key
            return nil
        end
        cached[key] = f
        return f
    end,
})

-- 真正需要行为的那些单独实现
local handlers = {}
local function wrap(name, impl)
    handlers[name] = impl
    rawset(cached, name, function(...)
        lvgl_calls[#lvgl_calls + 1] = name
        local impl_fn = handlers[name]
        if impl_fn then return impl_fn(...) end
    end)
end

local event_cbs = {}   -- obj.__id -> { [code] = fn }
local function fire(obj, code)
    local set = event_cbs[obj.__id]
    if not set then return end
    local fn = set[code]
    if fn then fn({ __code = code, __target = obj }) end
end

wrap("app_page", function() return new_obj("page") end)
wrap("obj_create", function(p) return new_obj("obj", p) end)
wrap("label_create", function(p) return new_obj("label", p) end)
wrap("btn_create", function(p) return new_obj("btn", p) end)
wrap("textarea_create", function(p) return new_obj("textarea", p) end)
-- 注意：lv_keyboard_create() 的真实实现（lv_keyboard.c:505）在构造函数里做了
--   lv_obj_align(obj, LV_ALIGN_BOTTOM_MID, 0, 0);
-- 也就是 align = BOTTOM_MID 且 y = 0。桩必须照抄这一点，否则测不出
-- "只 obj_set_pos 不重置 align 会把键盘推出屏幕" 这个 bug。
wrap("keyboard_create", function(p)
    local o = new_obj("keyboard", p)
    o.align, o.x, o.y = CONST.ALIGN_BOTTOM_MID, 0, 0
    return o
end)
wrap("label_set_text", function(o, t) o.text = t or ""; labels[o.text] = o end)
wrap("textarea_set_text", function(o, t) o.text = t or "" end)
wrap("textarea_get_text", function(o) return o.text end)
wrap("obj_get_width", function() return 320 end)
wrap("obj_get_height", function() return 240 end)
wrap("obj_set_size", function(o, w, h) o.w, o.h = w, h end)
wrap("obj_set_pos", function(o, x, y) o.x, o.y = x, y end)
wrap("obj_add_flag", function(o, f) if f == CONST.FLAG_HIDDEN then o.hidden = true end; o.flags[f] = true end)
wrap("obj_clear_flag", function(o, f) if f == CONST.FLAG_HIDDEN then o.hidden = false end; o.flags[f] = nil end)
wrap("obj_has_flag", function(o, f) return o.flags[f] == true end)
wrap("obj_set_style_bg_color", function(o, c) o.styles.bg = c end)
wrap("obj_set_style_bg_opa", function(o, v) o.styles.bg_opa = v end)
wrap("obj_set_style_text_color", function(o, c) o.styles.fg = c end)
wrap("obj_set_style_border_width", function(o, v) o.styles.border = v end)
wrap("obj_set_style_border_color", function(o, c) o.styles.border_color = c end)
wrap("obj_set_style_radius", function(o, v) o.styles.radius = v end)
wrap("obj_add_event_cb", function(o, fn, code)
    event_cbs[o.__id] = event_cbs[o.__id] or {}
    event_cbs[o.__id][code] = fn
end)
wrap("event_get_code", function(e) return e.__code end)
wrap("event_get_target", function(e) return e.__target end)
wrap("obj_move_to_index", function(o, i) o.moved_to = i end)
wrap("obj_align", function(o, align, x, y) o.align = align; o.x, o.y = x or 0, y or 0 end)
wrap("obj_center", function(o) o.align = CONST.ALIGN_CENTER; o.x, o.y = 0, 0 end)
wrap("obj_add_state", function(o, s) o.states = o.states or {}; o.states[s] = true end)
wrap("obj_clear_state", function(o, s) if o.states then o.states[s] = nil end end)
wrap("obj_has_state", function(o, s) return o.states ~= nil and o.states[s] == true end)
wrap("keyboard_set_textarea", function(kb, ta) kb.ta = ta end)
wrap("keyboard_set_candidate_list", function(kb, list) kb.cand = list end)
wrap("keyboard_set_mode", function(kb, m) kb.mode = m end)
wrap("keyboard_reset_ime", function(kb) kb.ime_reset = (kb.ime_reset or 0) + 1 end)

-- 子控件查询：sync_candidate_bar 用它对候选容器里的按钮矩阵
wrap("obj_get_child", function(o, idx) return o.children[(idx or 0) + 1] end)

-- batch：记录嵌套深度，用来断言"整屏刷新确实在一个事务里"
local batch_depth, batch_max, batch_calls = 0, 0, 0
local flushed_in_batch = nil
wrap("batch_begin", function()
    batch_depth = batch_depth + 1
    batch_calls = batch_calls + 1
    if batch_depth > batch_max then batch_max = batch_depth end
end)
wrap("batch_end", function()
    batch_depth = batch_depth - 1
    if batch_depth < 0 then batch_depth = 0 end
end)

lvgl = raw_lvgl

---------------------------------------------------------------- 桩：ui / rtos / sys
local notifications = {}
ui = {
    show_notification = function(msg, suffix, delay)
        notifications[#notifications + 1] = msg
    end,
    font14 = function() return nil end,
}

-- 时间：固定成 2026-11-22（周日）
local FIXED = { year = 2026, month = 11, day = 22, hour = 10, minute = 30,
                second = 0, weekday = 0, yearday = 326, isdst = false, unix = 0, valid = true }
rtos = {
    localTime = function() return FIXED end,
    timer_start = function() return 1 end,
    timer_stop = function() return 1 end,
    receive = function() return -1 end,
    MSG_TIMER = 0x100, INF_TIMEOUT = -1,
}

local loop_cb = nil
local timers = {}
sys = {
    taskInit = function(fn) end,
    wait = function() end,
    waitUntil = function() return false end,
    publish = function() end,
    subscribe = function() end,
    unsubscribe = function() end,
    timerStart = function(fn, ms) table.insert(timers, fn); return #timers end,
    timerLoopStart = function(fn, ms) loop_cb = fn; return 1 end,
    timerStop = function() end,
    run = function() end,
}
package.preload["sys"] = function() return sys end

---------------------------------------------------------------- 桩：fskv
local KV = {}
fskv = {
    init = function() return true end,
    set = function(k, v)
        if type(k) ~= "string" or k == "" then return false end
        if v == nil then return false end
        if type(v) == "string" and v:find("\0") then return false end
        KV[k] = v
        return true
    end,
    get = function(k) return KV[k] end,
    del = function(k) KV[k] = nil; return true end,
    clear = function() KV = {}; return true end,
    iter = function()
        local keys = {}
        for k in pairs(KV) do table.insert(keys, k) end
        if #keys == 0 then return nil end
        return { __keys = keys, __i = 0 }
    end,
    next = function(it)
        it.__i = it.__i + 1
        return it.__keys[it.__i]
    end,
}

---------------------------------------------------------------- 跑 main.lua
local ok, err = pcall(dofile, app .. "/main.lua")
check("main.lua 加载", ok, err)

---------------------------------------------------------------- 用到的 lvgl API 是否真实存在
-- 从 lvgl_bindings.txt + lua_lvgl.cpp 里取真实注册过的名字
local function real_api_names()
    local names = {}
    local f = io.open(app .. "/../../../components/lua_runtime/lvgl_bindings.txt", "r")
    if f then
        for raw_line in f:lines() do
            local line = raw_line:gsub("#.*$", ""):gsub("%s+$", ""):gsub("^%s+", "")
            if line ~= "" then names[line:gsub("^lv_", "")] = true end
        end
        f:close()
    end
    local cpp = io.open(app .. "/../../../components/lua_runtime/lua_lvgl.cpp", "r")
    if cpp then
        local body = cpp:read("a")
        cpp:close()
        for name in body:gmatch('lua_setfield%(L, %-2, "([%w_]+)"%)') do
            names[name] = true
        end
    end
    return names
end

local real = real_api_names()
local missing = {}
local checked = {}
for _, name in ipairs(lvgl_calls) do
    if not checked[name] then
        checked[name] = true
        if not real[name] and CONST[name] == nil then
            table.insert(missing, name)
        end
    end
end
check("用到的 lvgl API 都真实存在", #missing == 0, table.concat(missing, ", "))

-- 常量也要存在
for _, name in ipairs({ "PART_MAIN", "TEXT_ALIGN_LEFT", "TEXT_ALIGN_CENTER", "TEXT_ALIGN_RIGHT",
                        "LABEL_LONG_MODE_CLIP", "ALIGN_TOP_RIGHT", "ALIGN_BOTTOM_MID",
                        "FLAG_HIDDEN", "FLAG_SCROLLABLE", "FLAG_CLICKABLE", "DIR_HOR",
                        "OPA_TRANSP", "OPA_COVER", "EVENT_CLICKED", "EVENT_READY", "EVENT_CANCEL",
                        "EVENT_SCREEN_UNLOAD_START", "KEYBOARD_MODE_TEXT_LOWER" }) do
    check("lvgl." .. name .. " 被读到", CONST[name] ~= nil)
end

---------------------------------------------------------------- 界面断言
-- 找到各个控件：靠 label 文字反查
local function by_text(t) return labels[t] end
-- 按钮的事件回调注册在 btn 上，而 labels 里存的是 btn 里的那个 label，要往上找一层
local function btn_of(t)
    local lbl = labels[t]
    if not lbl then return nil end
    return lbl.__parent
end
local function click(t, quiet)
    local btn = btn_of(t)
    if not quiet then
        check("找得到按钮 " .. t, btn ~= nil)
    end
    if btn then fire(btn, CONST.EVENT_CLICKED) end
    return btn
end

check("月份标题", by_text("2026年11月") ~= nil)
check("星期表头 日", by_text("日") ~= nil)
check("星期表头 六", by_text("六") ~= nil)
check("今天按钮", by_text("今天") ~= nil)
check("返回按钮", by_text("返回") ~= nil)
check("编辑按钮", by_text("编辑") ~= nil)
check("保存按钮", by_text("保存") ~= nil)
check("删除按钮", by_text("删除") ~= nil)

-- 2026-11-01 是周日 -> 第一格就是 1 号
local grid = nil
for _, o in pairs(objects) do
    if o.__kind == "obj" and #o.children == 42 then grid = o end
end
check("42 个日期格", grid ~= nil)
if grid then
    local first = grid.children[1]
    local first_lbl = first.children[1]
    check_eq("第一格是 1 号", first_lbl.text, "1")
    -- 11 月 30 天，第 31 格是 12 月 1 日
    local cell31 = grid.children[31].children[1]
    check_eq("第 31 格是 12 月 1 日", cell31.text, "1")
    -- 越界格是灰色的
    check("越界格背景淡", grid.children[31].styles.bg == 0xEEF2F7)
end

-- 今天（11/22）实底高亮
local today_cell = nil
if grid then
    for _, cell in ipairs(grid.children) do
        if cell.children[1].text == "22" and cell.styles.bg == 0x1B54A6 then
            today_cell = cell
        end
    end
end
check("今天 11/22 实底高亮", today_cell ~= nil)

-- 摘要行：11月22日 周日 · 农历...
local sum1 = nil
local sum2 = nil
for _, o in pairs(objects) do
    if o.__kind == "label" and o.text and o.text:find("^11月22日 周日") then sum1 = o end
    if o.__kind == "label" and o.text and o.text == "点击日期查看详情或添加备注" then sum2 = o end
end
check("摘要第一行是选中日", sum1 ~= nil, sum1 and sum1.text)
check("摘要提示语", sum2 ~= nil)

---------------------------------------------------------------- 农历一致性
local lunar = require("lunar")
local info = lunar.fromSolar(2026, 11, 22)
check("农历 2026-11-22", info ~= nil)
if info and sum1 then
    local want = string.format(" · %s%s%s", info.isLeap and "闰" or "", info.monthCn, info.dayCn)
    check("摘要里的农历与 lib 一致", sum1.text:find(want, 1, true) ~= nil, sum1.text .. " / " .. want)
end

---------------------------------------------------------------- 点击日期 -> 详情
-- 找 11 月 25 日那一格（第 25 格，因为 11-01 是周日）
local cell25 = grid.children[25]
fire(cell25, CONST.EVENT_CLICKED)

local function find_label_containing(pattern)
    for _, o in pairs(objects) do
        if o.__kind == "label" and o.text and o.text:find(pattern) then return o end
    end
    return nil
end

check("详情页标题", find_label_containing("^11月25日") ~= nil)
local lunar_line = find_label_containing("^农历")
check("详情页农历行", lunar_line ~= nil, lunar_line and lunar_line.text)
if lunar_line then
    local d = lunar.fromSolar(2026, 11, 25)
    local want = string.format("农历%s%s%s", d.isLeap and "闰" or "", d.monthCn, d.dayCn)
    check_eq("详情页农历内容", lunar_line.text, want)
end
check("详情页干支行", find_label_containing("年 · 属") ~= nil)
check("详情页无备注提示", find_label_containing("还没有备注") ~= nil)

---------------------------------------------------------------- 备注：编辑 -> 保存
click("编辑")

-- 键盘应当显示，且 IME 被重置
local kb = nil
for _, o in pairs(objects) do
    if o.__kind == "keyboard" then kb = o end
end
check("键盘存在", kb ~= nil)
check("编辑时键盘显示", kb and kb.hidden == false)
check("编辑时设置了候选词容器", kb and kb.cand ~= nil)
check("编辑时 reset_ime 被调用", kb and (kb.ime_reset or 0) > 0)
check("编辑时 textarea 挂上键盘", kb and kb.ta ~= nil)

-- 没有候选词时候选条必须是隐藏的（否则空白一条挡视线）
check("没有候选词时候选条隐藏", kb and kb.cand and kb.cand.hidden == true)

-- 保存/取消按钮在编辑态可见，且不落在键盘区域里
check("编辑态有保存按钮", by_text("保存") ~= nil)
check("编辑态有取消按钮", by_text("取消") ~= nil)

-- 键盘的**实际绘制位置**必须在屏幕内。这是之前真机上"键盘不出现"的根因：
-- lv_keyboard_create() 内部设了 align = BOTTOM_MID + y = 0，只 obj_set_pos
-- 会被叠加成 240-96+144 = 288（屏幕外 48px）。这里按 LVGL 的真实坐标语义算绝对位置。
if kb then
    local kx, ky = abs_pos(kb)
    check("键盘宽度是整屏", kb.w == 320, tostring(kb.w))
    check("键盘高度合理", kb.h > 0, tostring(kb.h))
    check("键盘水平位置为 0", kx == 0, tostring(kx))
    check("键盘在屏幕内（底部对齐）", ky >= 0 and ky + kb.h <= 240,
        string.format("键盘 y=%d..%d, 屏幕高 240", ky, ky + kb.h))
    check("键盘贴屏幕底部", ky + kb.h == 240, string.format("键盘底 %d", ky + kb.h))

    local save_btn = by_text("保存").__parent
    local _, sy = abs_pos(save_btn)
    check("保存按钮在键盘上方", sy + save_btn.h <= ky,
        string.format("按钮底 %d, 键盘顶 %d", sy + save_btn.h, ky))

    local tx, ty = abs_pos(kb.ta)
    check("输入框在键盘上方", ty + kb.ta.h <= ky,
        string.format("输入框底 %d, 键盘顶 %d", ty + kb.ta.h, ky))
    check("输入框宽度够", kb.ta.w >= 300, tostring(kb.ta.w))
end
check("编辑时查看态的编辑按钮隐藏", by_text("编辑").__parent.hidden == true)
check("编辑时备注 caption 隐藏", by_text("备注").hidden == true)
check("编辑时输入框聚焦", lvgl.obj_has_state(kb.ta, CONST.STATE_FOCUSED))

-- 键盘「收起」只收键盘，留在编辑态；再点输入框要能弹回来。
-- （这是之前真机上"点输入框不弹键盘"的根因：LVGL 不会自己弹，必须应用接点击。）
fire(kb, CONST.EVENT_CANCEL)
check("收起键盘后键盘隐藏", kb.hidden == true)
check("收起键盘后仍在编辑态", by_text("保存").__parent.hidden == false)
check("收起键盘后输入框还可见", kb.ta.hidden == false)
check("收起键盘后候选条隐藏", kb.cand.hidden == true)

-- 点输入框 -> 键盘重新弹出
fire(kb.ta, CONST.EVENT_CLICKED)
check("点输入框后键盘弹出", kb.hidden == false)
check("点输入框后输入框聚焦", lvgl.obj_has_state(kb.ta, CONST.STATE_FOCUSED))

-- 再收一次，然后用键盘「完成」应保存并退出编辑态
fire(kb, CONST.EVENT_CANCEL)
check("第二次收起键盘", kb.hidden == true)

-- 重新弹出后测候选条
fire(kb.ta, CONST.EVENT_CLICKED)
check("再次点输入框键盘弹出", kb.hidden == false)

-- 模拟键盘产生了候选词：容器里出现可见的按钮矩阵
local fake_btnm = new_obj("btnmatrix", kb.cand)
fake_btnm.hidden = false
fake_btnm.flags[CONST.FLAG_HIDDEN] = nil
-- 跑几拍主循环，sync_candidate_bar 应该把候选条显示出来
for _ = 1, 10 do
    if loop_cb then loop_cb() end
end
check("有候选词时候选条显示", kb.cand.hidden == false)
check("候选条置顶", kb.cand.moved_to == -1)

-- 候选词消失后应当重新隐藏
fake_btnm.hidden = true
fake_btnm.flags[CONST.FLAG_HIDDEN] = true
for _ = 1, 10 do
    if loop_cb then loop_cb() end
end
check("候选词消失后候选条隐藏", kb.cand.hidden == true)

-- 键盘收起时即便还有候选词，候选条也不该留在屏幕上
fire(kb.ta, CONST.EVENT_CLICKED)
fake_btnm.hidden = false
fake_btnm.flags[CONST.FLAG_HIDDEN] = nil
for _ = 1, 10 do
    if loop_cb then loop_cb() end
end
check("键盘弹出时候选条跟随显示", kb.cand.hidden == false)
fire(kb, CONST.EVENT_CANCEL)
check("键盘收起时候选条一并隐藏", kb.cand.hidden == true)
fire(kb.ta, CONST.EVENT_CLICKED)

-- 输入内容并保存
local ta = kb.ta
ta.text = "团队周会 10:00"
click("保存")

check_eq("备注写入 fskv", KV["cal:n:2026-11-25"], "团队周会 10:00")
check("保存后键盘隐藏", kb.hidden == true)
check("保存后候选条隐藏", kb.cand.hidden == true)
check("保存后备注显示", find_label_containing("团队周会") ~= nil)
check("保存后编辑按钮回来了", by_text("编辑").__parent.hidden == false)

---------------------------------------------------------------- 月视图里出现小圆点
click("返回")
check("返回月视图", find_label_containing("^2026年11月") ~= nil)

-- 11/25 那格的圆点应当显示；11/26 的应当隐藏
local dot25 = grid.children[25].children[2]
local dot26 = grid.children[26].children[2]
check("有备注的格子显示圆点", dot25.hidden == false)
check("没备注的格子隐藏圆点", dot26.hidden == true)

-- 摘要第二行显示备注
local sum2_after = nil
for _, o in pairs(objects) do
    if o.__kind == "label" and o.text and o.text:find("^备注：") then sum2_after = o end
end
check("摘要显示备注", sum2_after ~= nil, sum2_after and sum2_after.text)

---------------------------------------------------------------- 删除备注
-- 再点一次 25 号进详情（返回月视图后格子还在，重新取一遍）
fire(grid.children[25], CONST.EVENT_CLICKED)
click("删除")
check("备注已从 fskv 删除", KV["cal:n:2026-11-25"] == nil)
check("删除后回到无备注提示", find_label_containing("还没有备注") ~= nil)

---------------------------------------------------------------- 翻月
click("返回")
click(">")
check("下个月标题", find_label_containing("^2026年12月") ~= nil)
click("<")
click("<")
check("上个月标题", find_label_containing("^2026年10月") ~= nil)

---------------------------------------------------------------- 今天按钮
click("今天")
check("回到本月", find_label_containing("^2026年11月") ~= nil)

---------------------------------------------------------------- 2 月 / 闰年边界（直接调 lunar，界面只保证不崩）
for _, y in ipairs({ 2024, 2025, 2026, 2100 }) do
    local days = lunar.solarDays(y, 2)
    check(string.format("%d 年 2 月有 %d 天", y, days), days == ((y % 4 == 0 and y % 100 ~= 0) or y % 400 == 0) and 29 or 28)
end

---------------------------------------------------------------- batch：整屏刷新必须在一次事务里
-- 翻页和切视图都要包 batch；这里数一下 lvgl 消息数与 batch 次数的关系。
local before_calls = #lvgl_calls
local before_batches = batch_calls
click(">")
local calls_per_flip = #lvgl_calls - before_calls
local batches_per_flip = batch_calls - before_batches
check("翻页把刷新包进了 batch", batches_per_flip >= 1, tostring(batches_per_flip))
-- 一次翻页要写 42 个格子 × 若干样式，消息数远大于 batch 次数
check("翻页的 lvgl 消息远多于 batch 次数", calls_per_flip > batches_per_flip * 10,
    string.format("%d 条消息 / %d 次 batch", calls_per_flip, batches_per_flip))
check("batch 没有泄漏（成对 begin/end）", batch_depth == 0, tostring(batch_depth))

---------------------------------------------------------------- 性能回归：翻一页不该再逐月/逐年累加
-- 优化前每月视图约 2.7ms（主机），优化后约 0.15ms。这里用宽松阈值防回归。
local t0 = os.clock()
for i = 1, 50 do
    click(">", true)
end
local flip_ms = (os.clock() - t0) * 1000
check("50 次翻页 < 100ms（主机）", flip_ms < 100, string.format("%.1f ms", flip_ms))

-- 农历累计表只建一次
local l2 = require("lunar")
local t1 = os.clock()
for i = 1, 200 do l2.fromSolar(2026, 11, 22) end
local conv_ms = (os.clock() - t1) * 1000
check("200 次农历换算 < 20ms（主机）", conv_ms < 20, string.format("%.1f ms", conv_ms))

print(string.format("\n%d passed, %d failed", passed, failed))
if failed > 0 then
    os.exit(1)
end
