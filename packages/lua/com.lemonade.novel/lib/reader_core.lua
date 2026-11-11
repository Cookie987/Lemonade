-- 小说阅读器核心逻辑：UTF-8 处理、分页、进度格式化。
-- 不依赖 LVGL / 文件系统，方便在电脑上做单元测试。

local M = {}

M.PAGE_LINES = 8          -- 每页行数
M.LINE_UNITS = 294        -- 正文可用宽度（像素）
M.READ_CHUNK = 32768      -- 每次从文件读取的字节数
M.MAX_BOOKS = 300
M.HISTORY_LIMIT = 12

local function trim(text)
    if not text or text == "" then
        return ""
    end
    return (text:gsub("^%s+", ""):gsub("%s+$", ""))
end

-- 返回 next_index, codepoint, byte_length；非法 UTF-8 返回 nil。
function M.utf8_next(text, index)
    local b1 = string.byte(text, index)
    if not b1 then
        return nil
    end
    if b1 < 0x80 then
        return index + 1, b1, 1
    end
    if b1 < 0xC2 or b1 > 0xF4 then
        return nil
    end
    if b1 <= 0xDF then
        local b2 = string.byte(text, index + 1)
        if not b2 or b2 < 0x80 or b2 > 0xBF then
            return nil
        end
        return index + 2, (b1 - 0xC0) * 0x40 + (b2 - 0x80), 2
    end
    if b1 <= 0xEF then
        local b2 = string.byte(text, index + 1)
        local b3 = string.byte(text, index + 2)
        if not b2 or not b3 or b2 < 0x80 or b2 > 0xBF or b3 < 0x80 or b3 > 0xBF then
            return nil
        end
        if b1 == 0xE0 and b2 < 0xA0 then
            return nil
        end
        if b1 == 0xED and b2 >= 0xA0 then
            return nil
        end
        return index + 3,
            (b1 - 0xE0) * 0x1000 + (b2 - 0x80) * 0x40 + (b3 - 0x80), 3
    end
    local b2 = string.byte(text, index + 1)
    local b3 = string.byte(text, index + 2)
    local b4 = string.byte(text, index + 3)
    if not b2 or not b3 or not b4 or b2 < 0x80 or b2 > 0xBF or b3 < 0x80 or b3 > 0xBF
        or b4 < 0x80 or b4 > 0xBF then
        return nil
    end
    if b1 == 0xF0 and b2 < 0x90 then
        return nil
    end
    if b1 == 0xF4 and b2 > 0x8F then
        return nil
    end
    return index + 4,
        (b1 - 0xF0) * 0x40000 + (b2 - 0x80) * 0x1000 + (b3 - 0x80) * 0x40 + (b4 - 0x80),
        4
end

-- 估算字符占用的显示宽度（像素），与 14px 的 Sarasa UI SC 对齐。
function M.char_units(codepoint)
    if codepoint == 9 then
        return 28
    end
    if codepoint < 0x20 then
        return 0
    end
    if codepoint == 32 then
        return 4
    end
    if codepoint >= 0x30 and codepoint <= 0x39 then
        return 9 -- 数字比普通半角略宽
    end
    if codepoint < 0x7F then
        return 8
    end
    if codepoint >= 0x0300 and codepoint <= 0x036F then
        return 0
    end
    if codepoint >= 0x2E80 then
        return 14
    end
    return 8
end

-- 校验 UTF-8。allow_partial 用于文件头这种可能从字符中间截断的片段。
function M.is_utf8(text, allow_partial)
    local i = 1
    local len = #text
    while i <= len do
        local b1 = string.byte(text, i)
        if not b1 then
            return false
        end
        if b1 < 0x80 then
            if (b1 < 0x20 and b1 ~= 9 and b1 ~= 10 and b1 ~= 13) or b1 == 0x7F then
                return false
            end
            i = i + 1
        elseif b1 >= 0xC2 and b1 <= 0xDF then
            if i + 1 > len then
                return allow_partial == true
            end
            local b2 = string.byte(text, i + 1)
            if b2 < 0x80 or b2 > 0xBF then
                return false
            end
            i = i + 2
        elseif b1 >= 0xE0 and b1 <= 0xEF then
            if i + 2 > len then
                return allow_partial == true
            end
            local b2 = string.byte(text, i + 1)
            local b3 = string.byte(text, i + 2)
            if b2 < 0x80 or b2 > 0xBF or b3 < 0x80 or b3 > 0xBF then
                return false
            end
            if b1 == 0xE0 and b2 < 0xA0 then
                return false
            end
            if b1 == 0xED and b2 >= 0xA0 then
                return false
            end
            i = i + 3
        elseif b1 >= 0xF0 and b1 <= 0xF4 then
            if i + 3 > len then
                return allow_partial == true
            end
            local b2 = string.byte(text, i + 1)
            local b3 = string.byte(text, i + 2)
            local b4 = string.byte(text, i + 3)
            if b2 < 0x80 or b2 > 0xBF or b3 < 0x80 or b3 > 0xBF or b4 < 0x80 or b4 > 0xBF then
                return false
            end
            if b1 == 0xF0 and b2 < 0x90 then
                return false
            end
            if b1 == 0xF4 and b2 > 0x8F then
                return false
            end
            i = i + 4
        else
            return false
        end
    end
    return true
end

-- 去掉 UTF-8 BOM，返回新字符串和 BOM 字节数。
function M.strip_bom(text)
    if text:sub(1, 3) == "\239\187\191" then
        return text:sub(4), 3
    end
    return text, 0
end

-- 将一段文本切成最多 max_lines 行的一页。
-- 返回 page_text, used_bytes, filled, line_count。
-- used_bytes 是本页在 chunk 中消耗的字节数；下一页应从 chunk 的 used_bytes + 1 开始。
-- filled 表示是否因为行数已满而结束（而不是因为 chunk 读完）。
-- 超出宽度的长行会在这里插入换行符，因此 LVGL 不需要再做换行。
-- want_text 为 false 时只计算边界，不拼接文本（用于扫描全书页数）。
function M.paginate(chunk, max_lines, line_units, want_text)
    max_lines = max_lines or M.PAGE_LINES
    line_units = line_units or M.LINE_UNITS
    if not chunk or chunk == "" then
        return "", 0, false, 0
    end

    local build_text = want_text ~= false
    local i = 1
    local len = #chunk
    local line_count = 0
    local units = 0
    local page_end = nil
    local output = build_text and {} or nil
    local line_parts = build_text and {} or nil

    local function flush_line()
        if build_text then
            output[#output + 1] = table.concat(line_parts)
            line_parts = {}
        end
        units = 0
    end

    while i <= len do
        local next_i, codepoint = M.utf8_next(chunk, i)
        if not next_i then
            break
        end

        if codepoint == 10 then
            flush_line()
            line_count = line_count + 1
            i = next_i
            if line_count >= max_lines then
                page_end = i
                break
            end
        elseif codepoint == 13 then
            i = next_i
        else
            local width = M.char_units(codepoint)
            if units + width > line_units and units > 0 then
                flush_line()
                line_count = line_count + 1
                if line_count >= max_lines then
                    page_end = i
                    break
                end
                -- 当前字符还没有被消费，下一轮会放到新行开头
            else
                if build_text then
                    line_parts[#line_parts + 1] = chunk:sub(i, next_i - 1)
                end
                units = units + width
                i = next_i
            end
        end
    end

    if not page_end then
        page_end = len + 1
        if units > 0 or (build_text and #line_parts > 0) then
            flush_line()
            line_count = line_count + 1
        end
    end

    local used = page_end - 1
    if used <= 0 then
        return "", 0, false, line_count
    end

    local text = build_text and table.concat(output, "\n") or ""
    return text, used, line_count >= max_lines, line_count
end

-- 32 位 FNV-1a。
function M.hash32(text)
    local hash = 0x811C9DC5
    for index = 1, #text do
        hash = (hash ~ text:byte(index)) * 0x01000193
    end
    return hash
end

-- 返回 8 位十六进制字符串，用作缓存文件名。
function M.hash_text(text)
    local hash = M.hash32(text)
    local high = (hash >> 16) & 0xFFFF
    local low = hash & 0xFFFF
    return string.format("%04x%04x", high, low)
end

function M.is_book_filename(name)
    if not name or name:sub(1, 1) == "." then
        return false
    end
    local ext = name:match("%.([^%.]+)$")
    return ext ~= nil and ext:lower() == "txt"
end

function M.title_from_path(path)
    local name = (path or ""):match("([^/\\]+)$") or (path or "")
    if M.is_book_filename(name) then
        name = name:gsub("%.[^%.]+$", "")
    end
    name = trim(name)
    if name == "" then
        name = path or ""
    end
    return name
end

function M.folder_from_path(path)
    local relative = (path or ""):gsub("\\", "/")
    relative = relative:gsub("^/sdcard/", "")
    local folder = relative:match("^(.*)/[^/]+$")
    if not folder or folder == "" then
        return "SD 卡"
    end
    return folder
end

function M.progress_key(path)
    return "novel:" .. (path or "")
end

function M.clamp_offset(offset, size)
    local value = math.floor(tonumber(offset) or 0)
    if value < 0 then
        value = 0
    end
    if size and value > size then
        value = size
    end
    return value
end

function M.percent(offset, size)
    if not size or size <= 0 then
        return "0%"
    end
    local value = offset / size * 100
    if value < 0 then
        value = 0
    elseif value > 100 then
        value = 100
    end
    if value > 0 and value < 1 then
        return "<1%"
    end
    return string.format("%d%%", math.floor(value + 0.5))
end

function M.percent_precise(offset, size)
    if not size or size <= 0 then
        return "0.0%"
    end
    local value = offset / size * 100
    if value < 0 then
        value = 0
    elseif value > 100 then
        value = 100
    end
    return string.format("%.1f%%", value)
end

return M
