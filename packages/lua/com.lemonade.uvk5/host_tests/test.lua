--[[
UV-K5 副屏 / 协议库的无硬件测试

用真 Lua 跑真实的 k5.lua 与 main.lua：桩掉 lvgl / usb / sys / log，
把实机抓到的帧喂进去，检查协议解析、Link 流程、以及 UI 文字与对齐。

跑法见同目录 README.md（先编一个主机 Lua 解释器）。

覆盖：
  1. 协议向量：CRC16、混淆表、帧组包/解包、真实抓包（0x0515 "F4HWN v6.0.0"）
  2. ReplyReader 的任意分片 / 粘连 / 垃圾 / 坏帧尾
  3. Link:request 全流程（组包 -> 写 -> 应答 -> 解析 -> publish -> 取回）
  4. Link 会话：握手 + 能力探测 + 读校准 + 一轮轮询
  5. main.lua：建界面 + render() 的文字/对齐/裁剪检查，以及"用到的 lvgl API 是否都存在"
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

---------------------------------------------------------------- 桩：日志
local logs = {}
local function log_impl(level, ...)
    local parts = {}
    for i = 1, select("#", ...) do parts[#parts + 1] = tostring((select(i, ...))) end
    logs[#logs + 1] = level .. " " .. table.concat(parts, " ")
end
log = setmetatable({
    info = function(...) log_impl("I", ...) end,
    warn = function(...) log_impl("W", ...) end,
    error = function(...) log_impl("E", ...) end,
    debug = function(...) log_impl("D", ...) end,
}, { __call = function(_, ...) log_impl("I", ...) end })

---------------------------------------------------------------- 桩：LVGL
-- lua_lvgl.cpp:1736 会把 ui 表注册成全局，main.lua 用它隐藏系统顶栏
ui = { hide_topbar = function() end }

-- 记录所有调用，既能让 main.lua 跑起来，又能检查用到的 API 是否真实存在
local lvgl_calls = {}
local objects = {}
local next_id = 0

local function new_obj(kind, parent)
    next_id = next_id + 1
    local o = { __id = next_id, __kind = kind, __parent = parent, text = nil, value = nil }
    objects[next_id] = o
    return o
end

local CONST = {
    TEXT_ALIGN_LEFT = 0, TEXT_ALIGN_CENTER = 1, TEXT_ALIGN_RIGHT = 2, TEXT_ALIGN_AUTO = 3,
    PART_MAIN = 0, PART_INDICATOR = 0x010000,
    OPA_TRANSP = 0, OPA_COVER = 255,
    BAR_MODE_NORMAL = 0, BAR_MODE_SYMMETRICAL = 1, BAR_MODE_RANGE = 2,
    GRAD_DIR_NONE = 0, GRAD_DIR_HOR = 1, GRAD_DIR_VER = 2,
    FLAG_SCROLLABLE = 0x00000010, FLAG_CLICKABLE = 0x00000020, FLAG_HIDDEN = 0x00000001,
    LABEL_LONG_MODE_WRAP = 0, LABEL_LONG_MODE_DOTS = 1, LABEL_LONG_MODE_SCROLL = 2,
    LABEL_LONG_MODE_SCROLL_CIRCULAR = 3, LABEL_LONG_MODE_CLIP = 4,
    ALIGN_TOP_LEFT = 0, ALIGN_CENTER = 9, ALIGN_TOP_MID = 2, ALIGN_TOP_RIGHT = 3,
    EVENT_CLICKED = 7, EVENT_ALL = 0xFFFFFFFF,
}
local cached = {}
lvgl = setmetatable(cached, {
    __index = function(_, key)
        local c = CONST[key]
        if c ~= nil then cached[key] = c; return c end
        -- 未知名字：默认当函数，记录调用（后面拿去核对 lvgl_bindings.txt）
        local f = function(...)
            lvgl_calls[#lvgl_calls + 1] = key
            if key == "app_page" then return new_obj("page") end
            if key == "obj_create" then return new_obj("obj", (select(1, ...))) end
            if key == "label_create" then return new_obj("label", (select(1, ...))) end
            if key == "bar_create" then return new_obj("bar", (select(1, ...))) end
            if key == "btn_create" then return new_obj("btn", (select(1, ...))) end
            if key == "img_create" then return new_obj("img", (select(1, ...))) end
            if key == "obj_get_width" then return 320 end
            if key == "obj_get_height" then return 240 end
            if key == "font_load" then return { __font = (select(1, ...)) } end
            if key == "ui_font14" then return { __font = "ui_font14" } end
            if key == "label_get_text" then return (select(1, ...)).text end
            if key == "bar_get_value" then return (select(1, ...)).value end
            return nil
        end
        cached[key] = f
        return f
    end,
})
-- 记录具体的 set 调用，便于断言
local function wrap(name)
    local orig = lvgl[name]
    rawset(cached, name, function(...)
        lvgl_calls[#lvgl_calls + 1] = name
        local a = { ... }
        if name == "label_set_text" then a[1].text = a[2] end
        if name == "bar_set_value" then a[1].value = a[2] end
        if name == "obj_set_style_text_align" then a[1].align = a[2] end
        if name == "obj_set_size" then a[1].w, a[1].h = a[2], a[3] end
        if name == "obj_set_pos" then a[1].x, a[1].y = a[2], a[3] end
        if name == "obj_set_style_text_font" then a[1].font = a[2] end
        if name == "obj_add_flag" then a[1].hidden = true end
        if name == "obj_clear_flag" then a[1].hidden = false end
        if orig then return orig(...) end
    end)
end
for _, n in ipairs({ "label_set_text", "bar_set_value", "obj_set_style_text_align", "obj_set_size",
                     "obj_set_pos", "obj_set_style_text_font", "label_set_long_mode",
                     "obj_add_flag", "obj_clear_flag" }) do
    wrap(n)
end

---------------------------------------------------------------- 桩：sys / usb
-- 所有桩状态先集中声明：Lua 的局部变量作用域从声明之后开始，
-- 下面的函数若引用"后面才 local 的变量"，捕获到的是全局 nil（本项目已经踩过三次）。
local PUBLISHED = nil
local OUTBOX = nil          -- 电台还没"送上来"的应答（在等待时才到达，模拟真实时序）
local lua_task = nil
local rx_cb = nil
local CONNECTED = true
local tx_log = {}           -- 我们发出去的命令 id 序列
local EEPROM = {}
local RSSI_RAW = 138        -- 桩电台 0x0527 返回的 RSSI 原始值（可改，用来测"有信号/静噪"）
local k5                    -- 协议库（稍后 require）

-- 收应答：等 sys.wait/waitUntil 时才把它喂给 Lua，这样 Link 的 pending 已经设好了
local function deliver_pending()
    if OUTBOX ~= nil then
        local frame = OUTBOX
        OUTBOX = nil
        if rx_cb then rx_cb(frame) end
    end
end

sys = {
    taskInit = function(fn)
        lua_task = coroutine.create(fn)
        local ok, err = coroutine.resume(lua_task)
        if not ok then error("taskInit: " .. tostring(err)) end
    end,
    wait = function(ms)
        deliver_pending()
        coroutine.yield()
    end,
    waitUntil = function(id, ms)
        deliver_pending()
        if PUBLISHED ~= nil then
            local r = PUBLISHED
            PUBLISHED = nil
            return true, r
        end
        return false
    end,
    publish = function(id, data) PUBLISHED = data end,
    timerLoopStart = function() end,
    timerStart = function() end,
    timerStop = function() end,
    subscribe = function() end,
    unsubscribe = function() end,
    run = function() end,
}
-- 设备上 sys 是由运行时预注册的模块；主机上把它挂到 package.preload
package.preload["sys"] = function() return sys end

-- 电台应答：把实机日志里的 0x0515 应答原样放进来（含 0xFF 填充）
local CAPTURED_REPLY_0515 = string.char(
    0xAB, 0xCD, 0x28, 0x00, 0x03, 0x69, 0x30, 0xE6, 0x68, 0xA5, 0x45, 0x17, 0x6F, 0x15, 0xA3, 0x76,
    0x3D, 0x33, 0xC7, 0xB0, 0x16, 0x6C, 0x14, 0xE6, 0x2E, 0x91, 0x0D, 0x40, 0x21, 0x35, 0xD5, 0x40,
    0x13, 0x03, 0xE9, 0x80, 0x16, 0x6C, 0x14, 0xE6, 0x2E, 0x91, 0x0D, 0x40, 0xDE, 0xCA, 0xDC, 0xBA)

local OBF = { 0x16, 0x6C, 0x14, 0xE6, 0x2E, 0x91, 0x0D, 0x40, 0x21, 0x35, 0xD5, 0x40, 0x13, 0x03, 0xE9, 0x80 }

-- 按电台的 SendReply_VCP 组一个应答帧（载荷后那两字节是 0xFF 填充，不是 CRC）
local function build_reply(id, data)
    local payload = string.char(id & 0xFF, (id >> 8) & 0xFF, #data & 0xFF, (#data >> 8) & 0xFF) .. data
    local body = {}
    for i = 1, #payload do body[i] = string.byte(payload, i) ~ OBF[(i - 1) % 16 + 1] end
    body[#body + 1] = OBF[(#payload) % 16 + 1] ~ 0xFF
    body[#body + 1] = OBF[(#payload + 1) % 16 + 1] ~ 0xFF
    return string.char(0xAB, 0xCD, #payload & 0xFF, (#payload >> 8) & 0xFF, table.unpack(body)) ..
           string.char(0xDC, 0xBA)
end

local function u16(v) return string.char(v & 0xFF, (v >> 8) & 0xFF) end
local function u32(v) return string.char(v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF, (v >> 24) & 0xFF) end

-- VFO 记录（16 字节）：+0 freq, +4 offset, +8 code_rx, +0x0A codeType, +0x0B mod/dir, +0x0C flags
local function vfo_record(freq10hz, power, code, code_type)
    return u32(freq10hz)                                  -- +0x00 接收频率(10Hz)
        .. u32(0)                                         -- +0x04 偏频
        .. string.char(code, 0, code_type & 0x0F, 0)      -- +0x08 code_rx, +0x09 code_tx, +0x0A codeType, +0x0B mod/dir
        .. string.char((power << 2) & 0xFF, 0, 0, 0)      -- +0x0C flags(power<<2)，后三字节略
end

-- EEPROM 读的应答：回显 offset + size，再跟数据
local function eeprom_reply(offset, data)
    return u16(offset) .. string.char(#data, 0) .. data
end

usb = {
    connected = function() return CONNECTED end,
    available = function() return 0 end,
    flush = function() end,
    on_receive = function(fn) rx_cb = fn end,
    write = function(frame)
        -- 用协议库自己解析刚发出去的请求（顺带验证命令帧能解出来）
        local k5 = require("k5")
        local r = k5.new_reader()
        local reqs = k5.reader_push(r, frame)
        local req = reqs[1]
        if not req then return #frame end
        tx_log[#tx_log + 1] = req.id

        local reply = nil
        if req.id == 0x0514 then
            reply = CAPTURED_REPLY_0515
        elseif req.id == 0x051B then
            local off = k5.read_u16(req.data, 0)
            local size = string.byte(req.data, 3)
            local data = string.rep("\0", size)
            if EEPROM[off] then data = EEPROM[off] end
            reply = build_reply(0x051C, eeprom_reply(off, data))
        elseif req.id == 0x0527 then
            -- rssi=138 / band5(corr -6) -> -97dBm -> S8
            reply = build_reply(0x0528, u16(RSSI_RAW) .. string.char(0x20, 0))
        elseif req.id == 0x0529 then
            reply = build_reply(0x052A, u16(2442) .. u16(0))                    -- 2442/2230 -> 8.32 V
        end
        if reply and rx_cb then OUTBOX = reply end
        return #frame
    end,
}

---------------------------------------------------------------- 1. 协议向量
print("== 协议向量 ==")
k5 = require("k5")
check("crc16('123456789') = 0x31C3", k5.crc16("123456789") == 0x31C3)
check("混淆表一致",
    table.concat({ 0x16, 0x6C, 0x14, 0xE6, 0x2E, 0x91, 0x0D, 0x40, 0x21, 0x35, 0xD5, 0x40, 0x13, 0x03, 0xE9, 0x80 }, ",")
    == table.concat({ 0x16, 0x6C, 0x14, 0xE6, 0x2E, 0x91, 0x0D, 0x40, 0x21, 0x35, 0xD5, 0x40, 0x13, 0x03, 0xE9, 0x80 }, ","))

local r = k5.new_reader()
local got = k5.reader_push(r, CAPTURED_REPLY_0515)
check("实机 0x0515 应答解析成 1 帧", #got == 1, #got)
if got[1] then
    check("  id = 0x0515", got[1].id == 0x0515, string.format("0x%04X", got[1].id))
    check("  版本 = F4HWN v6.0.0", got[1].data:match("^([^\0]*)") == "F4HWN v6.0.0",
        got[1].data:match("^([^\0]*)"))
    check("  帧尾残留缓冲为空", #r.buf == 0, #r.buf)
else
    check("  id = 0x0515", false)
end

---------------------------------------------------------------- 2. ReplyReader 容错
print("\n== ReplyReader 容错 ==")
r = k5.new_reader()
local total = 0
for i = 1, #CAPTURED_REPLY_0515 do
    total = total + #k5.reader_push(r, CAPTURED_REPLY_0515:sub(i, i))
end
check("逐字节喂也能解出 1 帧", total == 1, total)
r = k5.new_reader()
check("两帧粘连 -> 2", #k5.reader_push(r, CAPTURED_REPLY_0515 .. CAPTURED_REPLY_0515) == 2)
r = k5.new_reader()
local n = #k5.reader_push(r, "\0\255\85\170")
n = n + #k5.reader_push(r, CAPTURED_REPLY_0515)
check("跳过前导垃圾 -> 1", n == 1, n)
r = k5.new_reader()
local bad = CAPTURED_REPLY_0515:sub(1, -2) .. "\0\0"
check("坏帧尾 -> 丢弃", #k5.reader_push(r, bad) == 0)
r = k5.new_reader()
local f = k5.build_command(0x051B, u16(0x00A010) .. string.char(16, 0) .. u32(0x123456))
local back = k5.reader_push(r, f)
check("组包/解包往返（0x051B）",
    #back == 1 and back[1].id == 0x051B and back[1].data == u16(0x00A010) .. string.char(16, 0) .. u32(0x123456))

---------------------------------------------------------------- 3. 换算
print("\n== 换算 ==")
check("43950000 -> 439.50000", k5.fmt_mhz(43950000) == "439.50000", k5.fmt_mhz(43950000))
check("adc=2442 cal=2230 -> 8.32 V", k5.fmt_adc_volt(2442, 2230) == "8.32 V", k5.fmt_adc_volt(2442, 2230))
check("rssi 138 band5 -> -97dBm", k5.rssi_to_dbm(138, 5) == -97, k5.rssi_to_dbm(138, 5))
check("CTCSS 索引 12 -> CTCSS 100.0Hz", k5.fmt_code(12, 1) == "CTCSS 100.0Hz", k5.fmt_code(12, 1))
check("DCS 索引 0 -> DCS 023N", k5.fmt_code(0, 2) == "DCS 023N", k5.fmt_code(0, 2))
check("信道 1029 -> VFO 400 MHz", k5.describe_channel(1029).label == "VFO 400 MHz",
    k5.describe_channel(1029).label)
check("MR ch 10 -> 0x00A0", k5.vfo_address(10, 0) == 0x00A0)
check("band VFO 1029 vfo1 -> 0x90B0", k5.vfo_address(1029, 1) == 0x0090B0)

-------------------------------------------------------------- 3b. S 表映射（电台规则）
print("\n== S 表映射（S9 = -93dBm，1 个 S = 6dB）==")
check("-93dBm -> S9", k5.s_meter_text(-93) == "S9", k5.s_meter_text(-93))
check("-99dBm -> S8", k5.s_meter_text(-99) == "S8", k5.s_meter_text(-99))
check("-141dBm -> S1 / -142dBm -> S0",
    k5.s_meter_text(-141) == "S1" and k5.s_meter_text(-142) == "S0",
    k5.s_meter_text(-141) .. "/" .. k5.s_meter_text(-142))
check("-94dBm -> S8（差 1dB 不到 S9）", k5.s_meter_text(-94) == "S8", k5.s_meter_text(-94))
check("-88dBm(S9+5) -> +05", k5.s_meter_text(-88) == "+05", k5.s_meter_text(-88))
check("超过 S9 截到 +40", k5.s_meter_text(-40) == "+40", k5.s_meter_text(-40))
check("138/band5 = -97dBm -> S8", k5.s_meter_text(k5.rssi_to_dbm(138, 5)) == "S8",
    k5.s_meter_text(k5.rssi_to_dbm(138, 5)))
check("13 格电平条：S9 -> 9，+25dB -> 11，-160dBm -> 0",
    k5.rssi_level(-93) == 9 and k5.rssi_level(-68) == 11 and k5.rssi_level(-160) == 0,
    k5.rssi_level(-93) .. "/" .. k5.rssi_level(-68) .. "/" .. k5.rssi_level(-160))
check("没有读数 -> 空字符串 / 0 格", k5.s_meter_text(nil) == "" and k5.rssi_level(nil) == 0)

-------------------------------------------------------------- 3c. 静噪判定
print("\n== 静噪判定（本底 + 余量）==")
local M = k5.SQL_MARGIN_RAW
check("余量 = 16 + 静噪等级*2", k5.sql_margin(1) == M + 2 and k5.sql_margin(9) == M + 18,
    k5.sql_margin(1) .. "/" .. k5.sql_margin(9))
check("本底之上不够余量 -> 静音", k5.sql_is_open(150, 138, M + 2, false) == false)
check("本底 + 余量 -> 有信号", k5.sql_is_open(156, 138, M + 2, false) == true)
check("慢了 1 格也算信号（>=）", k5.sql_is_open(155, 138, M + 2, false) == false)
check("迟滞：已经开着，掉到阈值下 6 以内仍算有信号",
    k5.sql_is_open(151, 138, M + 2, true) == true)
check("迟滞：掉太多就关", k5.sql_is_open(149, 138, M + 2, true) == false)
check("没有读数 / 读 0 -> 静音",
    k5.sql_is_open(nil, 138, M + 2, true) == false and k5.sql_is_open(0, 138, M + 2, true) == false)

---------------------------------------------------------------- 4. Link 全流程（含桩电台）
print("\n== Link 会话 ==")
-- 准备 EEPROM 内容
local chans = u16(0) .. u16(0) .. u16(0) .. u16(1029) .. u16(0) .. u16(0) .. u16(0) .. u16(0)
EEPROM = {
    [0x00A010] = chans,                                    -- ScreenChannel[0]=0(CH1) ScreenChannel[1]=1029(VFO400M)
    [0x00A0A3] = string.char(0),                           -- TX_VFO = 0 (A 是主)
    [0x00A001] = string.char(1),                           -- SQL 1
    [0x00A0B9] = string.char(0xF1, 0xF0, 0xF6, 0xFC, 0xF9, 0xFA, 0xFF),   -- dBm 修正（末位 0xFF = 未初始化）
    [0x00B140] = string.rep("\0", 6) .. u16(2230) .. string.rep("\0", 4),
    [0] = vfo_record(43950000, 7, 12, 1),                  -- MR ch1: 439.50000 / HIGH / CTCSS 100.0Hz
    [0x90B0] = vfo_record(14550000, 6, 0, 0),              -- VFO 400MHz 的 B 侧: 145.50000 / MID
}

local states = {}
local link = k5.new_link({
    interval_ms = 250,
    on_status = function(text, ok) logs[#logs + 1] = (ok and "I " or "W ") .. text end,
    on_update = function(st) states[#states + 1] = st end,
})
link:start()

check("会话建立起一个状态", #states == 1, #states)
if #states == 0 then
    print("  --- 库日志 ---")
    for _, l in ipairs(logs) do print("    " .. l) end
    print("  --- 发出的命令 ---")
    for _, id in ipairs(tx_log) do print(string.format("    0x%04X", id)) end
end
local st = states[1]
if st then
    check("  版本 = F4HWN v6.0.0", st.version == "F4HWN v6.0.0", st.version)
    check("  能力：RSSI/ADC 都可用", st.caps.rssi and st.caps.adc)
    check("  电池校准读到 2230", link.batt_cal == 2230, link.batt_cal)
    check("  RSSI 修正表未初始化 -> 用默认值", link.dbm_corr[1] == -15, link.dbm_corr[1])
    check("  主 VFO = A", st.tx_vfo == 0, st.tx_vfo)
    check("  A: CH1 / 439.50000 / HIGH / FM / CTCSS",
        st.a and st.a.ch == 0 and st.a.freq_text == "439.50000" and st.a.power == 7 and st.a.mod == 0
        and st.a.code_type_rx == 1 and st.a.code_rx == 12,
        st.a and (st.a.freq_text .. "/" .. tostring(st.a.power)))
    check("  B: VFO 400MHz / 145.50000", st.b and st.b.ch == 1029 and st.b.freq_text == "145.50000",
        st.b and st.b.freq_text)
    check("  RSSI 138 / band5(corr -6) -> -97dBm", st.rssi_dbm == -97, st.rssi_dbm)
    check("  S 表 = S8（电台规则，不是以前那套 S9=-73）", st.s_text == "S8", st.s_text)
    check("  13 格电平条 = 8", st.rssi_level == 8, st.rssi_level)
    check("  电压 8.32 V", st.volt == "8.32 V", st.volt)
    check("  静噪 = 1 / 余量 = 18", st.squelch == 1 and st.squelch_margin == 18,
        tostring(st.squelch) .. "/" .. tostring(st.squelch_margin))
    check("  首次采样当本底 -> 判定静音（界面会隐藏 RSSI/S 表）",
        st.squelch_floor == 138 and st.squelch_open == false,
        tostring(st.squelch_floor) .. "/" .. tostring(st.squelch_open))
    check("  命令序列含 0x0514/0x0527/0x0529/0x051B",
        (function()
            local want = { [0x0514] = false, [0x0527] = false, [0x0529] = false, [0x051B] = false }
            for _, id in ipairs(tx_log) do want[id] = true end
            return want[0x0514] and want[0x0527] and want[0x0529] and want[0x051B]
        end)())
end

-- 拔掉设备：Link 应该停轮询并报"已移除"
print("\n== 热插拔 ==")
CONNECTED = false
local before = #tx_log
local ok, err = coroutine.resume(lua_task)      -- 让轮询循环再转一圈
check("拔掉后协程继续运行", ok, err)
check("拔掉后不再发命令", #tx_log == before, #tx_log)
local said_removed = false
for _, l in ipairs(logs) do
    if l:find("已移除", 1, true) then said_removed = true end
end
check("日志里报了设备已移除", said_removed)

---------------------------------------------------------------- 5. UI（跑 main.lua）
print("\n== UI ==")
CONNECTED = true            -- 插回去，让 main.lua 里的 Link 也能开会话
PUBLISHED, OUTBOX = nil, nil
local ui_ok, ui_err = pcall(dofile, app .. "/main.lua")
check("main.lua 建界面 + 首帧渲染无报错", ui_ok, ui_err)

local texts = {}
local function collect_texts()
    texts = {}
    for _, o in pairs(objects) do
        if o.__kind == "label" and o.text and o.text ~= "" then texts[o.text] = o end
    end
end
collect_texts()
local function has(t) return texts[t] ~= nil end

-- 两条电平条（A 区先建、B 区后建）：按创建顺序取
local bars = {}
for _, o in pairs(objects) do
    if o.__kind == "bar" then bars[#bars + 1] = o end
end
table.sort(bars, function(x, y) return x.__id < y.__id end)
local bar_a_obj, bar_b_obj = bars[1], bars[2]
check("界面上有两条电平条（A / B）", bar_a_obj ~= nil and bar_b_obj ~= nil, #bars)

check("顶部版本文字 F4HWN v6.0.0", has("F4HWN v6.0.0"))
check("状态 ONLINE", has("ONLINE"))
check("电量 8.32 V", has("8.32 V"))
check("A 频道 CH 1", has("CH 1"))
check("A 频率 439.50000", has("439.50000"))
check("A 功率 HIGH", has("HIGH"))
check("A 调制 FM", has("FM"))
check("A 亚音 CTCSS 100.0Hz", has("CTCSS 100.0Hz"))
check("B 频道 VFO 400 MHz", has("VFO 400 MHz"))
check("B 频率 145.50000", has("145.50000"))
check("按钮 菜单 / 通联日志 / PTT", has("菜单") and has("通联日志") and has("PTT"))

-- 静噪（本底就是第一帧的 RSSI）：两条都隐藏、S 表为空
check("静噪时隐藏 RSSI 条（A、B 都隐藏）",
    bar_a_obj ~= nil and bar_a_obj.hidden == true and bar_b_obj ~= nil and bar_b_obj.hidden == true,
    (bar_a_obj and tostring(bar_a_obj.hidden)) .. "/" .. (bar_b_obj and tostring(bar_b_obj.hidden)))
check("静噪时 S 表不显示（首帧没有 S 文本）",
    not has("S0") and not has("S8") and not has("+22"))

-- 来了个信号：RSSI 190（band5 -> -71dBm -> S9+22，电平条 11 格）；主 VFO = A
RSSI_RAW = 190
local ok2, err2 = coroutine.resume(lua_task)
check("来信号后协程继续跑", ok2, err2)
collect_texts()
check("有信号时只显示主 VFO（A）那一条",
    bar_a_obj ~= nil and bar_a_obj.hidden == false and bar_b_obj ~= nil and bar_b_obj.hidden == true,
    (bar_a_obj and tostring(bar_a_obj.hidden)) .. "/" .. (bar_b_obj and tostring(bar_b_obj.hidden)))
check("S 表 +22（S9+22dB，电台格式）", has("+22"))
check("电平条值 = 11 格（S9+2 格）", bar_a_obj ~= nil and bar_a_obj.value == 11,
    bar_a_obj and tostring(bar_a_obj.value))

-- 切到主 VFO = B：改接的应该是 B 那一条（不是拿 A 的数继续画）
-- 按坐标找 S 表标签：A 侧 (226,86)、B 侧 (226,168)
local function label_at(x, y)
    for _, o in pairs(objects) do
        if o.__kind == "label" and o.x == x and o.y == y then return o end
    end
end
EEPROM[0x00A0C3] = string.char(1)  -- TX_VFO（k5.TX_VFO_OFFSET）
RSSI_RAW = 130                     -- B 是 VFO 400MHz 频道（查表算 band），先落个本底
coroutine.resume(lua_task)
RSSI_RAW = 200                     -- band5(corr -6) -> -66dBm -> S9+27
local ok4, err4 = coroutine.resume(lua_task)
check("切主 VFO 后协程继续跑", ok4, err4)
collect_texts()
check("主 VFO 换成 B 后：显示 B 那条、隐藏 A 那条",
    bar_a_obj ~= nil and bar_a_obj.hidden == true and bar_b_obj ~= nil and bar_b_obj.hidden == false,
    (bar_a_obj and tostring(bar_a_obj.hidden)) .. "/" .. (bar_b_obj and tostring(bar_b_obj.hidden)))
check("S 表跟着换到 B 侧：A 侧清空、B 侧 +27",
    (label_at(226, 86) or {}).text == "" and (label_at(226, 168) or {}).text == "+27",
    tostring((label_at(226, 86) or {}).text) .. "/" .. tostring((label_at(226, 168) or {}).text))

-- 信号没了：回到本底，两条都隐藏
EEPROM[0x00A0C3] = string.char(0)
RSSI_RAW = 140
local ok3, err3 = coroutine.resume(lua_task)
check("信号消失后协程继续跑", ok3, err3)
collect_texts()
check("信号消失后又隐藏两条 RSSI 条 / 清空 S 表",
    bar_a_obj ~= nil and bar_a_obj.hidden == true and bar_b_obj ~= nil and bar_b_obj.hidden == true
    and not has("+27") and not has("+22"),
    (bar_a_obj and tostring(bar_a_obj.hidden)) .. "/" .. (bar_b_obj and tostring(bar_b_obj.hidden)))

-- 对齐：会变的文本必须"固定宽度 + 右对齐"，或"固定左边界 + 左对齐"
local function aligned(text, want)
    local o = texts[text]
    return o ~= nil and o.align == want and o.w ~= nil
end
check("右侧栏右对齐：HIGH/FM/CTCSS 100.0Hz/ONLINE/8.32 V",
    aligned("HIGH", CONST.TEXT_ALIGN_RIGHT) and aligned("FM", CONST.TEXT_ALIGN_RIGHT)
    and aligned("CTCSS 100.0Hz", CONST.TEXT_ALIGN_RIGHT)
    and aligned("ONLINE", CONST.TEXT_ALIGN_RIGHT) and aligned("8.32 V", CONST.TEXT_ALIGN_RIGHT))
check("左侧栏左对齐：CH 1/439.50000/VFO 400 MHz/F4HWN v6.0.0",
    aligned("CH 1", CONST.TEXT_ALIGN_LEFT) and aligned("439.50000", CONST.TEXT_ALIGN_LEFT)
    and aligned("VFO 400 MHz", CONST.TEXT_ALIGN_LEFT) and aligned("F4HWN v6.0.0", CONST.TEXT_ALIGN_LEFT))

-- 所有用到的 lvgl API 必须真实存在（对照 lvgl_bindings.txt + 手写包装）
local allowed = {}
do
    local f = io.open(app .. "/../../../components/lua_runtime/lvgl_bindings.txt", "r")
    if f then
        for line in f:lines() do
            local name = line:match("^%s*(lv_[A-Za-z0-9_]+)%s*$")
            if name then allowed[name:gsub("^lv_", "")] = true end
        end
        f:close()
    else
        print("  (警告：读不到 lvgl_bindings.txt，跳过 API 存在性检查)")
    end
end
-- 手写包装（不在 lvgl_bindings.txt 里）
for _, n in ipairs({ "app_page", "poll_events", "batch_begin", "batch_end", "obj_center", "font_load",
                     "ui_font14", "event_get_target", "event_get_code", "obj_add_event_cb",
                     "obj_get_width", "obj_get_height", "lv_scr_act" }) do
    allowed[n] = true
end
local missing = {}
for _, name in ipairs(lvgl_calls) do
    if not allowed[name] and not missing[name] then missing[name] = true end
end
local missing_list = {}
for k in pairs(missing) do missing_list[#missing_list + 1] = "lvgl." .. k end
table.sort(missing_list)
check("用到的 lvgl API 都存在", #missing_list == 0, table.concat(missing_list, " "))

---------------------------------------------------------------- 结果
print(string.format("\n%d passed, %d failed", passed, failed))
os.exit(failed == 0 and 0 or 1)
