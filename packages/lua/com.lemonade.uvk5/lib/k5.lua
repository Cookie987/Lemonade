--[[
UV-K1 / UV-K5 V3 命令-应答协议 —— Lua 移植版

移植自 ../uv-k1-k5v3-firmware-custom/tools/companion/index.html（Web Serial 副屏工具），
常量与换算全部照抄固件源码，注释里保留了原始出处。

用法（在协程里调用，例如 sys.taskInit 里）：

    local k5 = require("k5")
    local link = k5.new_link({
        interval_ms = 250,
        on_update = function(st) log.info("uvk5", st.a.freq_text) end,
        on_status = function(txt, ok) log.info("uvk5", txt) end,
    })
    link:start()
    sys.run()

link:start() 会自己开一个协程做：握手 -> 能力探测 -> 周期轮询 -> 回调。

依赖 C 侧的 `usb` 绑定（components/lua_runtime/lua_usb.cpp）：
    usb.write(s) -> n      发字节（经主循环转发到 USB CDC）
    usb.read(n, ms) -> s   读字节（ms=0 非阻塞；返回 nil 表示没数据）
    usb.available() -> n
    usb.flush()            丢弃缓冲
    usb.connected() -> bool
    usb.on_receive(fn)     异步：收到数据时在 Lua 任务里调用 fn(data)
]]

local sys = require("sys")

local k5 = {}

-- ---------------------------------------------------------------- 协议常量

-- App/app/uart.c:179-182
local OBFUSCATION = {
    0x16, 0x6C, 0x14, 0xE6, 0x2E, 0x91, 0x0D, 0x40,
    0x21, 0x35, 0xD5, 0x40, 0x13, 0x03, 0xE9, 0x80,
}

-- App/app/uart.c 命令分发表
k5.CMD = {
    DEV_INFO = 0x0514,          -- -> 0x0515
    DEV_INFO_RSP = 0x0515,
    READ_EEPROM = 0x051B,       -- -> 0x051C
    READ_EEPROM_RSP = 0x051C,
    READ_RSSI = 0x0527,         -- -> 0x0528（需 ENABLE_EXTRA_UART_CMD）
    READ_RSSI_RSP = 0x0528,
    READ_ADC = 0x0529,          -- -> 0x052A（需 ENABLE_EXTRA_UART_CMD）
    READ_ADC_RSP = 0x052A,
}

local CMD = k5.CMD

k5.EEPROM_READ_MAX = 128        -- App/app/uart.c:420

-- 扁平 EEPROM 地址（经 eeprom_compat.c 映射，主机只用这个空间）
k5.VFO_BASE = { 0x009000, 0x009010 }   -- settings.c:1227
k5.FREQ_CHANNEL_FIRST = 1024           -- misc.h:40/59
k5.VFO_SLOT_STRIDE = 32                -- radio.c:385
k5.VFO_RECORD_SIZE = 16
k5.SCREEN_CH_BASE = 0x00A010           -- settings.c:213（8 个 u16）
k5.TX_VFO_OFFSET = 0x00A0A8 + 0x18 + 3 -- settings.c:296
k5.SQUELCH_OFFSET = 0x00A000 + 1       -- settings.c:152
k5.DBMCORR_OFFSET = 0x00A0B9           -- settings.c:128（7 个 int8）
k5.BATT_CAL_OFFSET = 0x00B000 + 0x140  -- eeprom_compat.c:75 + settings.c:540

k5.MR_CHANNEL_LAST = 1023
k5.NOAA_CHANNEL_FIRST = 1024 + 7
k5.NOAA_CHANNEL_LAST = 1024 + 16
k5.MR_RECORD_SIZE = 16

-- VFO 记录内偏移（settings.c:1242-1266 / radio.c:394-470）
k5.VFO_OFF_FREQ = 0x00
k5.VFO_OFF_CODE_RX = 0x08
k5.VFO_OFF_CODE_TX = 0x09
k5.VFO_OFF_CODETYPE = 0x0A
k5.VFO_OFF_MOD = 0x0B
k5.VFO_OFF_FLAGS = 0x0C
k5.VFO_OFF_STEP = 0x0E

k5.DBMCORR_DEFAULT = { -15, -16, -10, -4, -7, -6, -1 }   -- misc.c:153

k5.BAND_NAMES = { "50 MHz", "108 MHz", "137 MHz", "174 MHz", "350 MHz", "400 MHz", "470 MHz" }

-- FREQUENCY_GetBand（frequencies.c:118-125），单位 10Hz；索引 6 固件里未定义
k5.BAND_LOWER_10HZ = {
    1800000, 10800000, 13700000, 17400000, 35000000, 40000000, 0, 47000000,
}

k5.POWER_NAMES = { "USER", "LOW 1", "LOW 2", "LOW 3", "LOW 4", "LOW 5", "MID", "HIGH" }  -- menu.c:207
k5.MOD_NAMES = { "FM", "AM", "USB", "LSB", "UKN" }

-- 码表索引（radio.c:287-303）：存的是索引不是频率
k5.CTCSS_OPTIONS = {
    670, 693, 719, 744, 770, 797, 825, 854, 885, 915,
    948, 974, 1000, 1035, 1072, 1109, 1148, 1188, 1230, 1273,
    1318, 1365, 1413, 1462, 1514, 1567, 1598, 1622, 1655, 1679,
    1713, 1738, 1773, 1799, 1835, 1862, 1899, 1928, 1966, 1995,
    2035, 2065, 2107, 2181, 2257, 2291, 2336, 2418, 2503, 2541,
}

k5.DCS_OPTIONS = {
    0x0013, 0x0015, 0x0016, 0x0019, 0x001A, 0x001E, 0x0023, 0x0027,
    0x0029, 0x002B, 0x002C, 0x0035, 0x0039, 0x003A, 0x003B, 0x003C,
    0x004C, 0x004D, 0x004E, 0x0052, 0x0055, 0x0059, 0x005A, 0x005C,
    0x0063, 0x0065, 0x006A, 0x006D, 0x006E, 0x0072, 0x0075, 0x007A,
    0x007C, 0x0085, 0x008A, 0x0093, 0x0095, 0x0096, 0x00A3, 0x00A4,
    0x00A5, 0x00A6, 0x00A9, 0x00AA, 0x00AD, 0x00B1, 0x00B3, 0x00B5,
    0x00B6, 0x00B9, 0x00BC, 0x00C6, 0x00C9, 0x00CD, 0x00D5, 0x00D9,
    0x00DA, 0x00E3, 0x00E6, 0x00E9, 0x00EE, 0x00F4, 0x00F5, 0x00F9,
    0x0109, 0x010A, 0x010B, 0x0113, 0x0119, 0x011A, 0x0125, 0x0126,
    0x012A, 0x012C, 0x012D, 0x0132, 0x0134, 0x0135, 0x0136, 0x0143,
    0x0146, 0x014E, 0x0153, 0x0156, 0x015A, 0x0166, 0x0175, 0x0186,
    0x018A, 0x0194, 0x0197, 0x0199, 0x019A, 0x01AC, 0x01B2, 0x01B4,
    0x01C3, 0x01CA, 0x01D3, 0x01D9, 0x01DA, 0x01DC, 0x01E3, 0x01EC,
}

-- ------------------------------------------------------------------ 工具

local byte = string.byte
local char = string.char
local floor = math.floor

function k5.u16le(v) return char(v & 0xFF, (v >> 8) & 0xFF) end

function k5.u32le(v)
    return char(v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF, (v >> 24) & 0xFF)
end

function k5.read_u16(b, o)     -- o 为 0-based 偏移，和 JS 版一致
    return byte(b, o + 1) | (byte(b, o + 2) << 8)
end

function k5.read_u32(b, o)
    return byte(b, o + 1) | (byte(b, o + 2) << 8) | (byte(b, o + 3) << 16) | (byte(b, o + 4) << 24)
end

function k5.hex(s, sep)
    if sep == nil then sep = " " end
    local t = {}
    for i = 1, #s do t[i] = string.format("%02X", byte(s, i)) end
    return table.concat(t, sep)
end

-- CRC-16/XMODEM（poly 0x1021, init 0）—— App/driver/crc.c
function k5.crc16(s)
    local crc = 0
    for i = 1, #s do
        crc = (crc ~ (byte(s, i) << 8)) & 0xFFFF
        for _ = 1, 8 do
            if (crc & 0x8000) ~= 0 then
                crc = ((crc << 1) ~ 0x1021) & 0xFFFF
            else
                crc = (crc << 1) & 0xFFFF
            end
        end
    end
    return crc
end

-- AB CD | size(u16 LE) | obf(payload + crc16) | DC BA
-- size = 载荷长度（id + dataLen + data），不含尾部 2 字节 CRC（uart.c:816-823）
function k5.build_command(id, data)
    data = data or ""
    local payload = k5.u16le(id) .. k5.u16le(#data) .. data
    local crc = k5.crc16(payload)
    local body = payload .. k5.u16le(crc)

    local out = { 0xAB, 0xCD, #payload & 0xFF, (#payload >> 8) & 0xFF }
    for i = 1, #body do
        out[#out + 1] = byte(body, i) ~ OBFUSCATION[(i - 1) % 16 + 1]
    end
    out[#out + 1] = 0xDC
    out[#out + 1] = 0xBA
    return char(table.unpack(out))
end

-- --------------------------------------------------------------- 应答解析

function k5.new_reader()
    return { buf = "" }
end

-- 取出一帧；不完整/错位返回 nil
function k5.reader_take(self)
    local buf = self.buf
    local start = string.find(buf, "\xAB\xCD", 1, true)
    if not start then
        if #buf > 1 then self.buf = buf:sub(#buf) end    -- 可能残留 AB
        return nil
    end
    if #buf - start + 1 < 4 then
        self.buf = buf:sub(start)
        return nil
    end

    local size = byte(buf, start + 2) | (byte(buf, start + 3) << 8)
    local total = 4 + size + 2 + 2                        -- 头 + (载荷+CRC) + 填充 + 尾

    if size > 4096 then                                   -- 明显错位：跳过这个候选头
        self.buf = buf:sub(start + 2)
        return nil
    end
    if #buf - start + 1 < total then
        self.buf = buf:sub(start)
        return nil
    end
    -- 帧尾检查：JS 的 0-based buf[start+total-2] / buf[start+total-1]
    -- 换成 1-based（start 已经 +1）就是 start+total-2 / start+total-1
    if byte(buf, start + total - 2) ~= 0xDC or byte(buf, start + total - 1) ~= 0xBA then
        self.buf = buf:sub(start + 2)
        return nil
    end

    local body = buf:sub(start + 4, start + 4 + size - 1)
    local plain = {}
    for k = 1, size do
        plain[k] = byte(body, k) ~ OBFUSCATION[(k - 1) % 16 + 1]
    end
    self.buf = buf:sub(start + total)

    if size < 4 then return nil end
    local id = plain[1] | (plain[2] << 8)
    local len = plain[3] | (plain[4] << 8)
    local data = string.char(table.unpack(plain, 5, 4 + len))
    return { id = id, data = data }
end

-- 塞入一段字节，返回解析出的所有应答
function k5.reader_push(self, chunk)
    self.buf = self.buf .. chunk
    if #self.buf > 8192 then self.buf = self.buf:sub(#self.buf - 2047) end
    local out = {}
    while true do
        local r = k5.reader_take(self)
        if not r then break end
        out[#out + 1] = r
    end
    return out
end

-- ------------------------------------------------------------ 显示/换算

-- 频率计数单位是 10Hz（ui/main.c:304 的 "%u.%05u"）
function k5.fmt_mhz(f)
    if not f or f == 0 or f < 0 or f > 2000000000 then return "--.-----" end
    return floor(f / 100000) .. "." .. string.format("%05d", f % 100000)
end

function k5.fmt_offset(f)
    if not f or f == 0 then return nil end
    return floor(f / 100000) .. "." .. string.format("%05d", f % 100000)
end

-- 0x0529 返回的是 12 位 ADC 原始计数（board.c:181），换算见 battery.c:148
function k5.fmt_adc_volt(raw, cal)
    cal = cal or 2000
    if raw == 0 or raw == 0xFFFF then return "-" end
    return string.format("%.2f V", floor((raw * 760) / cal + 0.5) / 100)
end

-- (raw/2) - 160 + dBmCorrTable[band]（bk4819.c:398-402 + rxtx_log.c:282-284）
function k5.rssi_to_dbm(raw, band, corr_table)
    if not raw then return nil end
    local tbl = corr_table or k5.DBMCORR_DEFAULT
    local idx = band or 0
    if idx < 0 then idx = 0 end
    if idx > #tbl then idx = #tbl end
    return floor(raw / 2) - 160 + (tbl[idx + 1] or 0)
end

-- ------------------------------------------------------------ S 表 / 静噪判定
--
-- 映射表是硬编码的，不需要从电台读任何东西：只要有 dBm 就能算。
-- 规则照抄电台自己的 S 表（ui/main.c:1111-1140，这台 987 版固件
-- ENABLE_RSSI_BAR=TRUE，走的是 F4HWN 那一支）：
--
--     S9 = -93 dBm，1 个 S 单位 = 6 dB，所以 S1 = -141 dBm、S0 = 低于 -141
--     s_level = (rssi_dBm + 147) / 6          -- 整数除法，范围 0..9
--     rssi_dBm >= -93  -> S9，超出的 dB 数另外显示（电台用 "+%02d"，最多 +40）
--     电平条 = s_level + overS9dBars（overS9dBars = overS9dBm / 10），满格 13
--
-- 注意 S9 不是 HF 那套 -73 dBm，也不是本工具以前用的 -73 —— 以电台屏幕为准。
k5.S9_DBM = -93
k5.S_MIN_DBM = -141           -- S1；低于它算 S0
k5.S_STEP_DB = 6
k5.S_MAX_OVER_DB = 40         -- 电台把超过 S9 的部分截到 +40（ui/main.c:1137）
k5.LEVEL_BAR_MAX = 13         -- ui/main.c:1187 DrawLevelBar(..., 13)

-- dBm -> { lv = S 等级 0..9, over = 超过 S9 的 dB 数(0..40) }
function k5.rssi_to_s(dbm)
    if not dbm then return nil end
    if dbm >= k5.S9_DBM then
        local over = dbm - k5.S9_DBM
        if over > k5.S_MAX_OVER_DB then over = k5.S_MAX_OVER_DB end
        return { lv = 9, over = over }
    end
    if dbm < k5.S_MIN_DBM then return { lv = 0, over = 0 } end
    return { lv = floor((dbm + 147) / k5.S_STEP_DB), over = 0 }
end

-- S 表文字，和电台屏幕一致："S0".."S9"，超过 S9 显示 "+NN"
function k5.s_meter_text(dbm)
    local s = k5.rssi_to_s(dbm)
    if not s then return "" end
    if s.over > 0 then return string.format("+%02d", s.over) end
    return "S" .. s.lv
end

-- 电台那条 13 格电平条的值（0 = 本底/静噪，9 = S9，13 = S9+40dB）
function k5.rssi_level(dbm)
    local s = k5.rssi_to_s(dbm)
    if not s then return 0 end
    return s.lv + floor(s.over / 10)
end

-- 静噪判定（决定界面要不要显示 RSSI/S 表）。
--
-- 命令接口拿不到"此刻静噪开还是关"（companion/README.md 第 4 条：固件没暴露这个
-- 瞬时状态），而电台的静噪是芯片里的**噪声门**、不是 RSSI 门 —— 所以它静音时
-- REG_67 依然读得到本底噪声（这台机器本底能到 -100dBm 上下，S 表看着像有信号）。
--
-- 这里照电台自己扫描器的做法（chFrScanner.c:264/471）：跟踪每个频段的 RSSI 本底，
-- 比本底高出一定余量才算"有信号"：
--     margin = SQL_MARGIN_RAW + 静噪等级 × SQL_LEVEL_STEP_RAW
-- 本底取"见过的最低值"：比它低就跟下去（一轮最多 SQL_DOWN_STEP_RAW）；只在"判定没信号"
-- 时慢慢往上抬（环境噪声变大能跟上，而长时间通话不会把本底抬上去）。抬的速度是每
-- ~5 秒 1 个原始值（0.1dB）—— 不能用"每次采样 +1"，那样本底会贴着噪声峰值走，
-- 弱信号永远不显示。
-- 关的时候要多掉 SQL_HYSTERESIS_RAW 才关，避免本底抖动时闪。
k5.SQL_MARGIN_RAW = 16          -- chFrScanner.c:264 SCAN_FAST_RSSI_MARGIN
k5.SQL_LEVEL_STEP_RAW = 2       -- 静噪每高一级，再多要 1dB
k5.SQL_HYSTERESIS_RAW = 6       -- 3dB 迟滞
k5.SQL_DOWN_STEP_RAW = 12       -- 一轮最多往下跟 6dB
k5.SQL_LEAK_MS = 5000           -- 本底往上抬一格要多久

function k5.sql_margin(level)
    return k5.SQL_MARGIN_RAW + (level or 0) * k5.SQL_LEVEL_STEP_RAW
end

-- 单个判断：raw = 本次 RSSI 原始值，floor = 本底，was_open = 上一轮是否算"有信号"
function k5.sql_is_open(raw, floor, margin, was_open)
    if not raw or raw == 0 or not floor then return false end
    if was_open then return raw >= floor + margin - k5.SQL_HYSTERESIS_RAW end
    return raw >= floor + margin
end

function k5.band_index_clamped(band, corr_table)
    local n = #(corr_table or k5.DBMCORR_DEFAULT)
    if band < 0 then return 0 end
    if band >= n then return n - 1 end
    return band
end

function k5.band_of_band_channel(ch)
    if ch >= k5.FREQ_CHANNEL_FIRST and ch < k5.FREQ_CHANNEL_FIRST + #k5.BAND_NAMES then
        return ch - k5.FREQ_CHANNEL_FIRST
    end
    return -1
end

function k5.band_of_frequency(freq10hz)
    for i = #k5.BAND_LOWER_10HZ, 1, -1 do
        local lower = k5.BAND_LOWER_10HZ[i]
        if lower > 0 and freq10hz >= lower then return i - 1 end
    end
    return 0
end

function k5.band_of(ch, freq10hz)
    local b = k5.band_of_band_channel(ch)
    if b >= 0 then return b end
    if freq10hz and freq10hz > 0 then return k5.band_of_frequency(freq10hz) end
    return 0
end

-- 记忆信道在电台屏幕上显示为"号 + 1"（ui/main.c:1698）
function k5.describe_channel(ch)
    if ch <= k5.MR_CHANNEL_LAST then
        return { kind = "mr", mode = "MR", label = "信道 " .. (ch + 1) }
    end
    if ch >= k5.NOAA_CHANNEL_FIRST and ch <= k5.NOAA_CHANNEL_LAST then
        return { kind = "noaa", mode = "MR", label = "气象 " .. (1 + ch - k5.NOAA_CHANNEL_FIRST) }
    end
    local band = ch - k5.FREQ_CHANNEL_FIRST
    if band >= 0 and band < #k5.BAND_NAMES then
        return { kind = "band", mode = "VFO", label = "VFO " .. k5.BAND_NAMES[band + 1] }
    end
    return { kind = "?", mode = "?", label = "信道 " .. (ch + 1) }
end

-- 索引 0 是合法码（CTCSS 67.0Hz / DCS 023N），只有 0xFF 才是无效哨兵
function k5.fmt_code(code, code_type)
    if code == nil or code == 0xFF or code_type == nil or code_type == 0 then return "" end
    if code_type == 1 then
        local hz10 = k5.CTCSS_OPTIONS[code + 1]
        if not hz10 then return "" end
        return "CTCSS " .. floor(hz10 / 10) .. "." .. (hz10 % 10) .. "Hz"
    end
    if code_type == 2 or code_type == 3 then
        local raw = k5.DCS_OPTIONS[code + 1]
        if not raw then return "" end
        -- 八进制打印（ui/main.c:2123）
        local oct = string.format("%o", raw)
        while #oct < 3 do oct = "0" .. oct end
        return "DCS " .. oct .. (code_type == 3 and "I" or "N")
    end
    return ""
end

-- VFO 记录地址：频段 VFO = VFO_BASE[vfo] + (ch-1024)*32，记忆信道 = ch*16
function k5.vfo_address(ch, vfo)
    if ch <= k5.MR_CHANNEL_LAST then
        return ch * k5.MR_RECORD_SIZE
    end
    local band = ch - k5.FREQ_CHANNEL_FIRST
    if band < 0 or band >= #k5.BAND_NAMES then return nil end
    return k5.VFO_BASE[vfo + 1] + band * k5.VFO_SLOT_STRIDE
end

-- ---------------------------------------------------------------- Link

local REPLY_MSG = "k5.reply"

local Link = {}
Link.__index = Link

-- opts: interval_ms, on_update(state), on_status(text, ok), on_log(text), quiet
function k5.new_link(opts)
    opts = opts or {}

    if type(usb) ~= "table" or type(usb.on_receive) ~= "function" then
        error("usb 绑定不可用：配置里需要有 usb_cdc_rx 组件（lua_runtime 会自动接线）", 2)
    end

    local link = setmetatable({
        interval_ms = opts.interval_ms or 250,
        on_update = opts.on_update,
        on_status = opts.on_status,
        on_log = opts.on_log,
        quiet = opts.quiet or false,
        reader = k5.new_reader(),
        pending = nil,
        running = false,
        caps = { rssi = false, adc = false },
        version = "",
        batt_cal = 2000,
        dbm_corr = k5.DBMCORR_DEFAULT,
        timestamp = 1,
        state = nil,
        fail = 0,
        last_hello = nil,
        rssi_floor = {},          -- 每个频段的 RSSI 本底（静噪判定用）
        sq_open = false,          -- 上一轮是否算"有信号"
        sql_leak = 0,             -- 本底缓慢抬高的计数器
        sql_leak_polls = math.max(1, floor(k5.SQL_LEAK_MS / (opts.interval_ms or 250))),
    }, Link)

    -- 异步接收：数据一到（Lua 任务里）就喂解析器
    usb.on_receive(function(chunk)
        if link.raw_log then link:log("RX " .. k5.hex(chunk)) end
        local replies = k5.reader_push(link.reader, chunk)
        if #replies == 0 then
            -- 自诊断：收到了字节却拆不出帧时把原始数据打出来（只报前 3 次）
            link.bad_chunks = (link.bad_chunks or 0) + 1
            if link.bad_chunks <= 3 then
                link:status(string.format("收到 %d 字节但未解析出帧（缓冲 %d 字节）：%s",
                    #chunk, #link.reader.buf, k5.hex(chunk)), false)
            end
        end
        for i = 1, #replies do
            local r = replies[i]
            if link.pending and link.pending.expect == r.id then
                link.pending = nil
                sys.publish(REPLY_MSG, r)
            elseif not link.quiet then
                link:log(string.format("忽略迟到应答 0x%04X", r.id))
            end
        end
    end)

    return link
end

function Link:log(msg)
    if self.on_log then
        self.on_log(msg)
    elseif not self.quiet then
        log.info("uvk5", msg)
    end
end

function Link:status(msg, ok)
    if self.on_status then
        self.on_status(msg, ok)
    elseif not self.quiet then
        if ok then log.info("uvk5", msg) else log.warn("uvk5", msg) end
    end
end

-- 发一条命令并等待应答；返回 reply 或 nil, err
-- 必须在协程里调用（sys.waitUntil 需要协程）
--
-- 等待切成 200 ms 小片：每片之间查一次 usb.connected()，这样设备被拔掉时
-- 不用等整个超时耗完就能立刻放弃（否则"已移除"要晚 1 秒多才报出来）。
function Link:request(id, data, expect_id, timeout_ms)
    if not usb.connected() then return nil, "USB 设备未连接" end

    local frame = k5.build_command(id, data)
    if self.raw_log then self:log("TX " .. k5.hex(frame)) end
    usb.write(frame)

    self.pending = { expect = expect_id }
    local budget = timeout_ms or 1200
    local waited = 0
    local reply, err = nil, nil

    while waited < budget do
        local slice = budget - waited
        if slice > 200 then slice = 200 end
        local ok, got = sys.waitUntil(REPLY_MSG, slice)
        if ok then
            reply = got
            break
        end
        waited = waited + slice
        if not usb.connected() then
            err = "USB 设备已移除"
            break
        end
    end

    self.pending = nil
    if reply then return reply end
    return nil, err or string.format("命令 0x%04X 超时", id)
end

-- 0x0514 握手：写入时间戳，0x051B 会强制校验它（uart.c:410-411）
-- 有效期 gSerialConfigCountDown_500ms = 12（约 6 秒），必须周期性续期
function Link:handshake()
    self.timestamp = math.random(1, 0xFFFFFF)
    local reply, err = self:request(CMD.DEV_INFO, k5.u32le(self.timestamp), CMD.DEV_INFO_RSP, 1500)
    if not reply then return nil, err end
    local ver = ""
    for i = 1, math.min(16, #reply.data) do
        local c = byte(reply.data, i)
        if c == 0 then break end
        ver = ver .. char(c)
    end
    self.version = ver
    return ver
end

function Link:read_eeprom(offset, size)
    size = size or 1
    if size > k5.EEPROM_READ_MAX then return nil, "单次读取最多 " .. k5.EEPROM_READ_MAX .. " 字节" end
    local d = k5.u16le(offset) .. char(size, 0) .. k5.u32le(self.timestamp)
    local reply, err = self:request(CMD.READ_EEPROM, d, CMD.READ_EEPROM_RSP, 1500)
    if not reply then return nil, err end
    if #reply.data < 4 then return nil, "读应答过短" end
    local len = byte(reply.data, 3) | (byte(reply.data, 4) << 8)
    return reply.data:sub(5, 4 + len)
end

function Link:read_rssi()
    local reply, err = self:request(CMD.READ_RSSI, "", CMD.READ_RSSI_RSP, 800)
    if not reply then return nil, err end
    if #reply.data < 4 then return nil, "RSSI 应答过短" end
    return { rssi = k5.read_u16(reply.data, 0), noise = byte(reply.data, 3), glitch = byte(reply.data, 4) }
end

function Link:read_adc()
    local reply, err = self:request(CMD.READ_ADC, "", CMD.READ_ADC_RSP, 800)
    if not reply then return nil, err end
    if #reply.data < 4 then return nil, "ADC 应答过短" end
    return { voltage = k5.read_u16(reply.data, 0), current = k5.read_u16(reply.data, 2) }
end

-- 探测 0x0527/0x0529 是否编译进固件（未编译时固件不回任何字节，只能靠超时）
function Link:probe_capabilities()
    self.caps = { rssi = false, adc = false }

    local rssi = self:read_rssi()
    if rssi then
        self.caps.rssi = true
        self.last_rssi = rssi.rssi
    else
        usb.flush()
    end

    local adc = self:read_adc()
    if adc then
        self.caps.adc = true
    else
        usb.flush()
    end

    return self.caps
end

-- 读一个 VFO 当前显示信道的记录
function Link:read_vfo(ch, vfo)
    local addr = k5.vfo_address(ch, vfo)
    if not addr then return nil end
    local rec = self:read_eeprom(addr, k5.VFO_RECORD_SIZE)
    if not rec or #rec < k5.VFO_RECORD_SIZE then return nil end

    local freq = k5.read_u32(rec, k5.VFO_OFF_FREQ)
    local mod_flags = byte(rec, k5.VFO_OFF_MOD + 1)
    local flags = byte(rec, k5.VFO_OFF_FLAGS + 1)
    local code_type = byte(rec, k5.VFO_OFF_CODETYPE + 1)

    return {
        ch = ch,
        freq = freq,
        freq_text = k5.fmt_mhz(freq),
        offset = k5.read_u32(rec, k5.VFO_OFF_FREQ + 4),
        offset_dir = mod_flags & 0x0F,          -- 0=关 1=加 2=减
        code_rx = byte(rec, k5.VFO_OFF_CODE_RX + 1),
        code_tx = byte(rec, k5.VFO_OFF_CODE_TX + 1),
        code_type_rx = code_type & 0x0F,
        code_type_tx = code_type >> 4,
        mod = mod_flags >> 4,
        power = (flags >> 2) & 7,
        width = (flags >> 1) & 1,
        band = k5.band_of(ch, freq),
    }
end

function Link:ch_text(info)
    if not info then return "?" end
    local d = k5.describe_channel(info.ch)
    return d.label
end

-- 一轮轮询：和 index.html 的 tick() 同一顺序
function Link:poll()
    -- 先续期握手（0x051B 校验时间戳），串行执行避免抢总线
    self:ensure_handshake()

    local rssi, rssi_raw = nil, nil
    if self.caps.rssi then
        local r = self:read_rssi()
        if r then
            rssi_raw = r.rssi
            self.last_rssi = r.rssi
            self.last_rssi_at = self.poll_count
        end
    end

    local chans, err = self:read_eeprom(k5.SCREEN_CH_BASE, 16)
    if not chans then error(err) end
    local ch0 = k5.read_u16(chans, 0)
    local ch1 = k5.read_u16(chans, 6)

    local txv = self:read_eeprom(k5.TX_VFO_OFFSET, 1)
    local tx_vfo = (txv and byte(txv, 1) < 2) and byte(txv, 1) or 0

    local info0 = self:read_vfo(ch0, 0)
    local info1 = self:read_vfo(ch1, 1)

    local sq = self:read_eeprom(k5.SQUELCH_OFFSET, 1)
    self.squelch = (sq and byte(sq, 1) < 10) and byte(sq, 1) or 1

    local volt = nil
    if self.caps.adc then
        local adc = self:read_adc()
        if adc then volt = k5.fmt_adc_volt(adc.voltage, self.batt_cal) end
    end

    -- RSSI 保持：射频芯片省电休眠时 REG_67 会读到 0
    if self.caps.rssi then
        if rssi_raw and rssi_raw > 0 then
            rssi = rssi_raw
            self.hold = { value = rssi_raw, at = self.poll_count }
        elseif self.hold and (self.poll_count - self.hold.at) * self.interval_ms < 4000 then
            rssi = self.hold.value
            self.hold_used = true
        else
            self.hold_used = false
        end
    end

    local act = (info0 and tx_vfo == 0) and info0 or info1
    local band = act and act.band or 0
    local dbm = rssi and k5.rssi_to_dbm(rssi, band, self.dbm_corr) or nil

    -- 有信号 / 静噪（见 k5.sql_margin 上面的说明）：本底按频段分别跟踪
    local margin = k5.sql_margin(self.squelch)
    local floor_raw = self.rssi_floor[band]
    if rssi_raw and rssi_raw > 0 then
        -- 读不到数（省电读 0）时不动本底：那是"没数据"，不是"没信号"
        if floor_raw == nil then
            floor_raw = rssi_raw                                   -- 该频段第一次采样：直接当本底
            self.sql_leak = 0
        elseif rssi_raw < floor_raw then
            -- 更低就跟下去，但一轮最多降 SQL_DOWN_STEP_RAW（6dB）：
            -- 单个异常低值（AGC 复位之类）不会把本底拖坏，环境真变安静几轮也就追上了
            floor_raw = floor_raw - math.min(k5.SQL_DOWN_STEP_RAW, floor_raw - rssi_raw)
            self.sql_leak = 0
        elseif not self.sq_open then
            self.sql_leak = (self.sql_leak or 0) + 1               -- 只在"没信号"时慢慢抬高
            if self.sql_leak >= self.sql_leak_polls then
                self.sql_leak = 0
                floor_raw = floor_raw + 1
            end
        end
        self.rssi_floor[band] = floor_raw
    end
    local open = k5.sql_is_open(rssi_raw, floor_raw, margin, self.sq_open)

    local state = {
        version = self.version,
        online = true,
        tx_vfo = tx_vfo,
        tx_vfo_name = tx_vfo == 0 and "A" or "B",
        a = info0,
        b = info1,
        rssi = rssi,
        rssi_held = self.hold_used or false,
        rssi_dbm = dbm,
        rssi_s = k5.rssi_to_s(dbm),
        rssi_level = k5.rssi_level(dbm),             -- 电台那条 13 格电平条
        s_text = k5.s_meter_text(dbm),               -- "S0".."S9" / "+NN"
        squelch = self.squelch,                      -- 静噪等级 0..9
        squelch_floor = floor_raw,                   -- 该频段的 RSSI 本底（原始值）
        squelch_margin = margin,                     -- 本底之上多少算"有信号"
        squelch_open = open,                         -- false = 电台静音，界面隐藏 RSSI/S 表
        volt = volt,
        power = act and k5.POWER_NAMES[act.power + 1] or nil,
        mod = act and k5.MOD_NAMES[act.mod + 1] or nil,
        caps = self.caps,
    }
    self.sq_open = open
    self.state = state
    self.fail = 0

    if self.on_update then self.on_update(state) end
    return state
end

function Link:ensure_handshake()
    -- 4 秒续期一次（0x0514/0x051B 都会重置 6 秒倒计时）
    if self.last_hello and (self.poll_count - self.last_hello) * self.interval_ms < 4000 then
        return true
    end
    local ver, err = self:handshake()
    if not ver then
        self:status("握手续期失败：" .. tostring(err), false)
        return false
    end
    self.last_hello = self.poll_count
    return true
end

-- 一次"会话"：设备在的时候要做的事 —— 握手 + 探测 + 读校准值
-- （对应 index.html 的 btnConnect 流程；每次插拔后都会重新走一遍握手）
function Link:open_session()
    if not usb.connected() then return false end

    usb.flush()
    self.pending = nil
    self.fail = 0

    local ver, err = self:handshake()
    if not ver then
        self:status("握手失败：" .. tostring(err), false)
        return false
    end
    self:status("握手成功，固件版本：" .. (ver ~= "" and ver or "(空)"), true)

    -- 能力探测要花两次超时（不支持时），只在第一次做，之后沿用结果。
    -- 只有两项都确认可用才缓存 —— 刚枚举完电台可能还没准备好，没探测到就下次会话再试。
    if not self.caps_probed then
        local caps = self:probe_capabilities()
        self.caps_probed = caps.rssi and caps.adc
        if caps.rssi then
            self:status("RSSI 可用（0x0527）", true)
        else
            self:status("RSSI 不可用：固件未编译 0x0527（需要 -DENABLE_EXTRA_UART_CMD=ON）", false)
        end
        if caps.adc then
            self:status("电池电压可用（0x0529）", true)
        else
            self:status("电池电压不可用：固件未编译 0x0529", false)
        end
    end

    local corr = self:read_eeprom(k5.DBMCORR_OFFSET, 7)
    if corr and #corr >= 7 then
        local tbl, ok = {}, true
        for i = 1, 7 do
            local v = byte(corr, i)
            if v == 0xFF then ok = false break end
            v = v % 256                                    -- 按无符号取
            if v > 127 then v = v - 256 end                -- int8
            tbl[i] = v
        end
        if ok then
            self.dbm_corr = tbl
            self:status("频段 RSSI 修正表：" .. table.concat(tbl, ","), true)
        else
            self:status("RSSI 修正表未初始化，用默认值 " .. table.concat(k5.DBMCORR_DEFAULT, ","), false)
        end
    else
        self:status("读 RSSI 修正表失败，用默认值", false)
    end

    if self.caps.adc then
        local cal = self:read_eeprom(k5.BATT_CAL_OFFSET, 12)
        if cal and #cal >= 8 then
            local c3 = k5.read_u16(cal, 6)
            if c3 >= 1500 and c3 <= 3500 then
                self.batt_cal = c3
                self:status("电池校准系数：" .. c3, true)
            else
                self:status("校准系数异常（" .. c3 .. "），用默认 " .. self.batt_cal, false)
            end
        else
            self:status("读校准失败，用默认 " .. self.batt_cal, false)
        end
    end

    self.last_hello = self.poll_count or 0
    self.online = true
    return true
end

-- 兼容旧名字
Link.connect = Link.open_session

-- 自己开协程跑：等设备 -> 会话 -> 周期轮询；插拔自动跟
--
-- 设备检测：每轮循环看一次 usb.connected()（就是 usb_uart 通道的 initialised_ ——
-- 设备枚举且 CDC 配置完成才为 true，移除立刻为 false）。
--   * 上升沿（插入/启动时已在位） -> open_session()：flush + 握手 + 探测 + 读校准
--   * 下降沿（移除）             -> 立即停轮询，不再发命令（发了只会白等超时）
--   * 连接着但握手失败           -> 每 2 秒重试，直到成功
function Link:start()
    if self.running then return end
    self.running = true
    self.online = false

    sys.taskInit(function()
        local tick = 0
        local present = false      -- 上一轮看到的设备状态，用来取边沿
        local retry_at = 0         -- 握手失败后的重试时刻（tick）
        local announced_wait = false

        while self.running do
            tick = tick + 1
            local connected = usb.connected()

            if connected ~= present then
                present = connected
                if connected then
                    self:status("USB 设备已插入，开始握手…", true)
                    retry_at = 0
                else
                    self.online = false
                    self.pending = nil
                    self:status("USB 设备已移除，暂停轮询（插回后自动重新握手）", false)
                end
            elseif not connected and not announced_wait then
                announced_wait = true
                self:status("等待 USB CDC 设备…（确认 USB 主机模式已打开、设备已供电）", true)
            end

            if connected and not self.online and tick >= retry_at then
                self.poll_count = 0
                if self:open_session() then
                    self:status("在线：" .. (self.version ~= "" and self.version or "未知版本"), true)
                else
                    -- 设备在，但电台没应答：2 秒后再试（可能刚枚举完还没准备好）
                    retry_at = tick + math.max(1, floor(2000 / self.interval_ms))
                end
            end

            if connected and self.online then
                self.poll_count = self.poll_count + 1
                self:ensure_handshake()
                local ok, err = pcall(function() self:poll() end)
                if not ok then
                    -- 拔线导致的失败不算错误：下一轮会走到"已移除"分支
                    if not string.find(tostring(err), "已移除", 1, true) then
                        self.fail = self.fail + 1
                        if self.fail <= 3 then
                            self:status("轮询错误：" .. tostring(err), false)
                        elseif self.fail == 4 then
                            self:status("连续失败，后续相同错误不再输出", false)
                        end
                    end
                end
            end

            sys.wait(self.interval_ms)
        end
        self.online = false
    end)
end

function Link:stop()
    self.running = false
end

return k5
