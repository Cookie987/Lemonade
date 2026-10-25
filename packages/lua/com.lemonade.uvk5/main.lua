--[[
UV-K1 / UV-K5 V3 副屏（UI 版）

版面参考 packages/lua/uvk5_ui（PicoPixel 生成的 320x240 设计）：
顶栏（状态/电量）、A/B 两个频段区（徽标 + 频道 + 大字频率 + 13 格电平条 + S 表）、
中间白色分隔线、底部三个按钮。

频率整行显示（9 个字符 "438.50000"，窄了会截断），功率标签挪到电平条那一行。

S 表 / 电平条按电台自己的规则（S9 = -93dBm、1 个 S = 6dB、13 格电平条），
映射表在 lib/k5.lua 里硬编码；只有主 VFO 那一侧画（芯片只有一个瞬时 RSSI，
拿不到另一路的），电台静噪（静音）时两侧都收起来 —— 那时 REG_67 读到的
只是本底噪声，显示出来等于把噪声当信号。

数据来自 lib/k5.lua 的 Link（0x0514 握手 + 0x051B/0x0527/0x0529 轮询 + 热插拔自动重连），
本文件只负责画界面。

关于字体（assets/*.bin，都是 LVGL 二进制字体）：
  freq72.bin / freq64.bin  -> 数字 + "."  "+"  "-"  "/"（所以无效频率能画 --.-----）
  jetbrains20.bin          -> ASCII（含空格、+、-）
  chinese18.bin            -> 中文（按钮那几句；若以后加了"信道/气象"等字，把 CH_TEXT_CJK 打开即可）
排版要点见下面 mk_label 的注释：所有会变的文本都"固定宽度 + 左右对齐 + 超出裁剪"，
     这样内容长度变化时左右边界不会跟着动。
]]

local sys = require("sys")
local lv = lvgl
local k5 = require("k5")

local floor = math.floor

-- 先声明，后面再赋值：这样下面所有函数/闭包引用的都是这个局部变量。
-- （写成 local link = k5.new_link({ on_update = function() ... link ... end }) 会踩坑：
--   Lua 局部变量的作用域从赋值语句之后才开始，构造参数里的闭包会捕获全局 link = nil。）
local link

-- ------------------------------------------------------------------ 可调参数

local INTERVAL_MS = 80          -- 轮询间隔（UI 用 250ms；纯日志可以 1000）
local RAW_LOG = false            -- true: 把收发原始字节打进日志（联调用）
local CH_TEXT_CJK = false        -- true: 频道名用中文（信道 1 / 气象 3），需要 chinese18.bin 有这些字

local FONT_FILES = {
    freq_a = "assets/freq72.bin",
    freq_b = "assets/freq64.bin",
    small = "assets/jetbrains20.bin",
    cn = "assets/chinese18.bin",
}

-- 配色取自参考设计 uvk5_ui/colors.c
local C = {
    bg = 0x121212,
    bar = 0x3d3d3d,
    dim = 0x959393,
    red = 0xad2b2b,          -- A/B 徽标底色
    bar_red = 0xc44a4a,      -- RSSI 条渐变终点
    white = 0xffffff,
    green = 0x35c759,        -- 在线
}

-- 版面坐标（按 320x240 设计，运行时按 app page 实际尺寸换算）
-- 右栏宽度按"最长的文本 + 余量"给：jetbrains20 字宽 12px，
-- 例如 "CTCSS 100.0Hz" 12 字 = 144px、"8.32 V" 6 字 = 72px、"VFO 400 MHz" 11 字 = 132px。
--
-- 频率是 9 个字符（"438.50000"，电台刻度到 10Hz），freq72/freq64 都放不下 200px，
-- 所以两行频率都占满整行（x=6..314），功率标签挪到下面那条电平条那一行。
local L = {
    top_h = 22,
    ver = { 6, 2, 150, 19 },        -- 左对齐（"F4HWN v6.0.0" = 12 字 = 144px）
    state = { 156, 2, 84, 19 },     -- 右对齐（ONLINE / WAIT / OFFLINE）
    batt = { 238, 2, 76, 19 },      -- 右对齐（"8.32 V" = 6 字 = 72px）

    a_badge = { 6, 26, 22, 22 },
    a_ch = { 32, 27, 140, 19 },     -- 左对齐（"VFO 400 MHz" = 132px）
    a_cs = { 154, 27, 160, 19 },    -- 右对齐（"CTCSS 100.0Hz" = 144px / "DCS 023N" / 空）
    a_freq = { 6, 46, 308, 43 },    -- 整行，freq72 行高 43；万一 72 号字还是放不下 9 位数，
                                    -- 把下面 mk_label 的 F.freq_a 换成 F.freq_b（64 号字）
    a_bar = { 6, 94, 148, 11 },     -- 13 格电平条（和电台一样，0 格 = 静噪）
    a_pwr = { 210, 90, 64, 19 },    -- 右对齐（USER / LOW 1 / MID / HIGH）
    a_s = { 158, 90, 42, 19 },      -- 右对齐（S0..S9 / +NN，最多 3 字）
    a_mod = { 272, 90, 42, 19 },    -- 右对齐（FM / AM / USB / LSB）

    div1 = { 0, 111, 320, 3 },
    div2 = { 0, 186, 320, 3 },

    b_badge = { 6, 114, 22, 22 },
    b_ch = { 32, 115, 140, 19 },
    b_mod = { 164, 115, 150, 19 },
    b_freq = { 6, 132, 308, 39 },   -- 整行，freq64 行高 39
    b_bar = { 6, 174, 148, 9 },
    b_pwr = { 210, 168, 64, 19 },
    b_s = { 158, 168, 42, 19 },

    btn = { y = 191, h = 42, w = 100, xs = { 3, 110, 217 } },
}

-- ------------------------------------------------------------------ 字体

local function load_bin(path)
    local ok, font = pcall(lv.font_load, path)
    if ok and font ~= nil then return font end
    log.warn("uvk5", "字体加载失败：" .. path .. "（用默认字体）")
    return nil
end

local F = {
    freq_a = load_bin(FONT_FILES.freq_a),
    freq_b = load_bin(FONT_FILES.freq_b),
    small = load_bin(FONT_FILES.small),
    cn = load_bin(FONT_FILES.cn),
}

-- ------------------------------------------------------------------ 界面

local page = lv.app_page()
if not page then
    log.error("uvk5", "拿不到 app page，UI 无法建立")
    return
end

ui.hide_topbar()

lv.obj_add_event_cb(page, function(e)
    local code = lv.event_get_code(e)
    if code == lv.EVENT_SCREEN_LOAD_START then
        ui.hide_topbar()
    end
end, lv.EVENT_SCREEN_LOAD_START)

local PW = lv.obj_get_width(page)
local PH = lv.obj_get_height(page)
local sx, sy = PW / 320, PH / 240
local function X(v) return floor(v * sx) end
local function Y(v) return floor(v * sy) end
local function W(v) return floor(v * sx) end
local function H(v) return floor(v * sy) end

-- 建标签：
--   * 固定宽度 + 固定位置 -> 内容长度变化不会移动另一侧的边界
--   * 左右对齐用 text_align，右侧栏一律右对齐（右边界钉死在 X+W）
--   * LONG_MODE_CLIP -> 文字超长就裁掉，绝不换行把它自己撑大
local function mk_label(parent, box, align, font, color)
    local o = lv.label_create(parent)
    lv.obj_set_pos(o, X(box[1]), Y(box[2]))
    lv.obj_set_size(o, W(box[3]), H(box[4]))
    lv.label_set_long_mode(o, lv.LABEL_LONG_MODE_CLIP)
    lv.obj_set_style_text_align(o, align, lv.PART_MAIN)
    if font ~= nil then
        lv.obj_set_style_text_font(o, font, lv.PART_MAIN)
    end
    lv.obj_set_style_text_color(o, color, lv.PART_MAIN)
    return o
end

local function mk_rect(parent, box, color)
    local o = lv.obj_create(parent)
    lv.obj_set_pos(o, X(box[1]), Y(box[2]))
    lv.obj_set_size(o, W(box[3]), H(box[4]))
    lv.obj_clear_flag(o, lv.FLAG_SCROLLABLE)
    lv.obj_set_style_radius(o, 0, lv.PART_MAIN)
    lv.obj_set_style_border_width(o, 0, lv.PART_MAIN)
    lv.obj_set_style_pad_all(o, 0, lv.PART_MAIN)
    lv.obj_set_style_bg_color(o, color, lv.PART_MAIN)
    lv.obj_set_style_bg_opa(o, lv.OPA_COVER, lv.PART_MAIN)
    return o
end

local function mk_badge(parent, box, text, font)
    local o = mk_rect(parent, box, C.red)
    local l = lv.label_create(o)
    lv.label_set_text(l, text)
    if font ~= nil then
        lv.obj_set_style_text_font(l, font, lv.PART_MAIN)
    end
    lv.obj_set_style_text_color(l, C.white, lv.PART_MAIN)
    lv.obj_center(l)
    return o
end

local function mk_bar(parent, box, radius)
    local b = lv.bar_create(parent)
    lv.obj_set_pos(b, X(box[1]), Y(box[2]))
    lv.obj_set_size(b, W(box[3]), H(box[4]))
    -- 和电台一样是 13 格电平条：0 = 本底/静噪，9 = S9，13 = S9+40dB
    -- （ui/main.c:1187 DrawLevelBar(s_level + overS9Bars, 13)）
    lv.bar_set_range(b, 0, k5.LEVEL_BAR_MAX)
    lv.bar_set_mode(b, lv.BAR_MODE_NORMAL)
    lv.bar_set_value(b, 0, false)
    lv.obj_set_style_radius(b, radius, lv.PART_MAIN)
    lv.obj_set_style_bg_color(b, C.bar, lv.PART_MAIN)
    lv.obj_set_style_bg_opa(b, lv.OPA_COVER, lv.PART_MAIN)
    lv.obj_set_style_border_width(b, 0, lv.PART_MAIN)
    lv.obj_set_style_pad_all(b, 0, lv.PART_MAIN)
    lv.obj_set_style_radius(b, radius, lv.PART_INDICATOR)
    lv.obj_set_style_bg_color(b, C.white, lv.PART_INDICATOR)
    lv.obj_set_style_bg_grad_color(b, C.bar_red, lv.PART_INDICATOR)
    lv.obj_set_style_bg_grad_dir(b, lv.GRAD_DIR_HOR, lv.PART_INDICATOR)
    lv.obj_set_style_bg_opa(b, lv.OPA_COVER, lv.PART_INDICATOR)
    return b
end

-- 按钮（暂时只做外观，不接功能）
local function mk_button(parent, x, w, text, font)
    local b = lv.btn_create(parent)
    lv.obj_set_pos(b, X(x), Y(L.btn.y))
    lv.obj_set_size(b, W(w), H(L.btn.h))
    lv.obj_set_style_bg_color(b, C.bar, lv.PART_MAIN)
    lv.obj_set_style_bg_opa(b, lv.OPA_COVER, lv.PART_MAIN)
    lv.obj_set_style_border_color(b, C.white, lv.PART_MAIN)
    lv.obj_set_style_border_width(b, 2, lv.PART_MAIN)
    lv.obj_set_style_radius(b, 0, lv.PART_MAIN)
    lv.obj_set_style_pad_all(b, 0, lv.PART_MAIN)
    local l = lv.label_create(b)
    lv.label_set_text(l, text)
    if font ~= nil then
        lv.obj_set_style_text_font(l, font, lv.PART_MAIN)
    end
    lv.obj_set_style_text_color(l, C.white, lv.PART_MAIN)
    lv.obj_center(l)
    return b
end

lv.batch_begin()
lv.obj_clean(page)
lv.obj_clear_flag(page, lv.FLAG_SCROLLABLE)
lv.obj_set_style_pad_all(page, 0, lv.PART_MAIN)
lv.obj_set_style_border_width(page, 0, lv.PART_MAIN)
lv.obj_set_style_bg_color(page, C.bg, lv.PART_MAIN)
lv.obj_set_style_bg_opa(page, lv.OPA_COVER, lv.PART_MAIN)

-- 顶栏
mk_rect(page, { 0, 0, 320, L.top_h }, C.bar)
local lbl_ver = mk_label(page, L.ver, lv.TEXT_ALIGN_LEFT, F.small, C.dim)
local lbl_state = mk_label(page, L.state, lv.TEXT_ALIGN_RIGHT, F.small, C.dim)
local lbl_batt = mk_label(page, L.batt, lv.TEXT_ALIGN_RIGHT, F.small, C.white)
lv.label_set_text(lbl_state, "WAIT")
lv.label_set_text(lbl_ver, "")

-- A 区
mk_badge(page, L.a_badge, "A", F.small)
local lbl_a_ch = mk_label(page, L.a_ch, lv.TEXT_ALIGN_LEFT, F.small, C.white)
local lbl_a_cs = mk_label(page, L.a_cs, lv.TEXT_ALIGN_RIGHT, F.small, C.dim)
local lbl_a_freq = mk_label(page, L.a_freq, lv.TEXT_ALIGN_LEFT, F.freq_a or F.small, C.white)
local lbl_a_pwr = mk_label(page, L.a_pwr, lv.TEXT_ALIGN_RIGHT, F.small, C.white)
local bar_a = mk_bar(page, L.a_bar, 5)
local lbl_a_s = mk_label(page, L.a_s, lv.TEXT_ALIGN_RIGHT, F.small, C.dim)
local lbl_a_mod = mk_label(page, L.a_mod, lv.TEXT_ALIGN_RIGHT, F.small, C.white)

-- 分隔线
mk_rect(page, L.div1, C.white)

-- B 区
mk_badge(page, L.b_badge, "B", F.small)
local lbl_b_ch = mk_label(page, L.b_ch, lv.TEXT_ALIGN_LEFT, F.small, C.white)
local lbl_b_mod = mk_label(page, L.b_mod, lv.TEXT_ALIGN_RIGHT, F.small, C.white)
local lbl_b_freq = mk_label(page, L.b_freq, lv.TEXT_ALIGN_LEFT, F.freq_b or F.small, C.dim)
local bar_b = mk_bar(page, L.b_bar, 4)
local lbl_b_pwr = mk_label(page, L.b_pwr, lv.TEXT_ALIGN_RIGHT, F.small, C.white)
local lbl_b_s = mk_label(page, L.b_s, lv.TEXT_ALIGN_RIGHT, F.small, C.dim)

-- 分隔线 + 按钮
mk_rect(page, L.div2, C.white)
mk_button(page, L.btn.xs[1], L.btn.w, "菜单", F.cn)
mk_button(page, L.btn.xs[2], L.btn.w, "通联日志", F.cn)
mk_button(page, L.btn.xs[3], L.btn.w, "PTT", F.small)

lv.batch_end()

-- ------------------------------------------------------------------ 文本换算

-- 频率：字体里有 "-" 了，无效时直接给 --.-----
local function freq_text(info)
    if not info then return "-" end
    return k5.fmt_mhz(info.freq)
end

-- 频道名：默认 ASCII（"VFO 400 MHz" / "CH 1" / "WX 3"）；
-- CH_TEXT_CJK = true 时用库里的中文标签（需要 chinese18.bin 有"信道/气象"等字）
local function ch_text(info)
    if not info then return "" end
    local ch = info.ch
    if not ch then return "" end
    if CH_TEXT_CJK then return k5.describe_channel(ch).label end
    if ch <= k5.MR_CHANNEL_LAST then return "CH " .. (ch + 1) end
    if ch >= k5.NOAA_CHANNEL_FIRST and ch <= k5.NOAA_CHANNEL_LAST then
        return "WX " .. (1 + ch - k5.NOAA_CHANNEL_FIRST)
    end
    local name = k5.BAND_NAMES[ch - k5.FREQ_CHANNEL_FIRST + 1]
    if name then return "VFO " .. name end
    return "CH " .. (ch + 1)
end

local function mod_text(info)
    if not info then return "" end
    return k5.MOD_NAMES[(info.mod or 0) + 1] or ""
end

local function power_text(info)
    if not info then return "" end
    return k5.POWER_NAMES[(info.power or 0) + 1] or ""
end

local function code_text(info)
    if not info then return "" end
    return k5.fmt_code(info.code_rx, info.code_type_rx)
end

-- dBm -> S 表文字由 k5.lua 负责（映射表硬编码在那边）：S9 = -93dBm、1 个 S = 6dB，
-- 超过 S9 显示 "+NN"，电平条是电台那套 13 格。这里不再自己算 ——
-- 以前用 S9 = -73dBm（HF 习惯）那套是错的。

-- ------------------------------------------------------------------ 刷新

-- RSSI 电平条 + S 表：只有"电台此刻在接收的那一路"才有数据 ——
-- 0x0527 读的是芯片 REG_67，整台机器只有一个射频芯片、一个瞬时 RSSI，
-- 而 RX_VFO（正在收哪一路）只在 RAM 里、EEPROM 和命令接口都拿不到
-- （companion/README.md 第 4 条）。电台自己屏幕上也是只画当前这一路。
-- 所以：主 VFO 那一侧画条 + S 表，另一侧整组收起来 —— 不拿主 VFO 的数去填另一路。
-- 静噪（静音）时两边都不画。
-- 先声明再赋值：下面 render 引用的必须是这些局部变量
local shown_a, shown_b = nil, nil

local function set_bar_visible(bar, side_shown, show)
    if show == side_shown then return side_shown end
    if show then
        lv.obj_clear_flag(bar, lv.FLAG_HIDDEN)
    else
        lv.obj_add_flag(bar, lv.FLAG_HIDDEN)
    end
    return show
end

-- 两边都不显示（静噪、断线、没有数据时）
local function hide_signal()
    shown_a = set_bar_visible(bar_a, shown_a, false)
    shown_b = set_bar_visible(bar_b, shown_b, false)
    lv.label_set_text(lbl_a_s, "")
    lv.label_set_text(lbl_b_s, "")
end

local function refresh_state_label()
    local text, color
    if link.online then
        text, color = "ONLINE", C.green
    elseif usb.connected() then
        text, color = "WAIT", C.dim
    else
        text, color = "OFFLINE", C.red
    end
    lv.label_set_text(lbl_state, text)
    lv.obj_set_style_text_color(lbl_state, color, lv.PART_MAIN)
end

-- 上一轮的静噪状态，只用来在边沿打日志（方便对着电台调 k5.SQL_MARGIN_RAW）
local last_sq_open = nil

local function render(st)
    lv.label_set_text(lbl_ver, st.version or "")
    lv.label_set_text(lbl_batt, st.volt or "")

    lv.label_set_text(lbl_a_ch, ch_text(st.a))
    lv.label_set_text(lbl_a_freq, freq_text(st.a))
    lv.label_set_text(lbl_a_pwr, power_text(st.a))
    lv.label_set_text(lbl_a_mod, mod_text(st.a))
    lv.label_set_text(lbl_a_cs, code_text(st.a))

    lv.label_set_text(lbl_b_ch, ch_text(st.b))
    lv.label_set_text(lbl_b_freq, freq_text(st.b))
    lv.label_set_text(lbl_b_pwr, power_text(st.b))
    lv.label_set_text(lbl_b_mod, mod_text(st.b))

    -- 电台静音时（静噪关着、没收到信号）REG_67 读到的只是本底噪声，
    -- 显示出来等于把噪声当信号；另外只有主 VFO 那一路有数据（见上面的说明）。
    local open = st.squelch_open and st.rssi_dbm ~= nil
    local show_a = open and st.tx_vfo == 0
    local show_b = open and st.tx_vfo == 1
    if st.squelch_open ~= last_sq_open then
        last_sq_open = st.squelch_open
        -- 这条日志是调 k5.SQL_MARGIN_RAW / SQL_LEVEL_STEP_RAW 的依据：
        -- 本底 raw + 余量 = 门槛，RSSI raw 要超过它才算"有信号"
        log.info("uvk5", string.format("静噪判定：%s（静噪 %s，本底 raw %s + 余量 %s = %s，本次 RSSI %s/%s dBm %s）",
            st.squelch_open and "有信号" or "静音",
            tostring(st.squelch), tostring(st.squelch_floor), tostring(st.squelch_margin),
            tostring((st.squelch_floor or 0) + (st.squelch_margin or 0)),
            tostring(st.rssi), tostring(st.rssi_dbm), st.s_text or ""))
    end
    shown_a = set_bar_visible(bar_a, shown_a, show_a)
    shown_b = set_bar_visible(bar_b, shown_b, show_b)
    lv.bar_set_value(bar_a, show_a and (st.rssi_level or 0) or 0, false)
    lv.bar_set_value(bar_b, show_b and (st.rssi_level or 0) or 0, false)
    lv.label_set_text(lbl_a_s, show_a and (st.s_text or "") or "")
    lv.label_set_text(lbl_b_s, show_b and (st.s_text or "") or "")

    -- 主 VFO 用亮色，另一路压暗（和参考设计里 A 亮 B 暗一致）
    lv.obj_set_style_text_color(lbl_a_freq, st.tx_vfo == 0 and C.white or C.dim, lv.PART_MAIN)
    lv.obj_set_style_text_color(lbl_b_freq, st.tx_vfo == 1 and C.white or C.dim, lv.PART_MAIN)

    refresh_state_label()
end

-- ------------------------------------------------------------------ 数据

-- 注意：link 已经在文件开头声明过了（见上面的说明），这里只是赋值。
link = k5.new_link({
    interval_ms = INTERVAL_MS,
    on_status = function(text, ok)
        -- 库里的状态文本是中文（日志用），界面上的状态字用 ASCII 自己算
        if ok then
            log.info("uvk5", text)
        else
            log.warn("uvk5", text)
        end
        if not link.online then
            -- 设备拔了/断线：没有新数据，把上一次的 RSSI 条和 S 表收起来（留着就是假数据）
            lv.batch_begin()
            hide_signal()
            refresh_state_label()
            lv.batch_end()
            last_sq_open = nil
        else
            refresh_state_label()
        end
    end,
    on_update = function(st)
        lv.batch_begin()
        render(st)
        lv.batch_end()
    end,
})

link.raw_log = RAW_LOG
link:start()

sys.timerLoopStart(function()
    lv.poll_events(10)
end, 16)

sys.run()
