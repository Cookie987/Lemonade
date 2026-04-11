local sys = require("sys")
local lv = lvgl
local http = require("http")
local json = require("json")

local page = lv.app_page()
if not page then return end

lv.obj_clean(page)
lv.obj_set_style_bg_color(page, 0xF4F7FA, 0)

local ipa_font = lv.font_load("assets/ipa_14px.bin")
if not ipa_font then
    ui.show_notification("IPA 字体加载失败", "", 2000)
    return
end

local API_BASE = "https://v2.xxapi.cn/api/englishwords?word="
local SECTION_COLOR = "1B54A6"
local history_limit = 4
local KEYBOARD_OK = "\239\128\140"
local KEYBOARD_CLOSE = "\239\128\141"
local KEYBOARD_SYMBOL = "\239\132\156"

local state = {
    history = {},
    keyboard_visible = false,
}

local refs = {}

local function with_batch(fn)
    lv.batch_begin()
    local ok, result = pcall(fn)
    lv.batch_end()
    if not ok then
        error(result)
    end
    return result
end

local function trim(text)
    if not text then
        return ""
    end
    return (text:gsub("^%s+", ""):gsub("%s+$", ""))
end

local function urlencode(text)
    return (text:gsub("([^%w%-_%.~])", function(ch)
        return string.format("%%%02X", string.byte(ch))
    end))
end

local function join_parts(parts, sep)
    local buffer = {}
    for _, part in ipairs(parts) do
        if part and part ~= "" then
            buffer[#buffer + 1] = part
        end
    end
    return table.concat(buffer, sep or " ")
end

local function set_word_summary(title, phonetic)
    with_batch(function()
        lv.label_set_text(refs.word_label, title or "英语词典")
        lv.label_set_text(refs.phonetic_label, phonetic or "输入英文单词后点击查询")
    end)
end

local function set_result_text(text)
    with_batch(function()
        lv.label_set_text(refs.result_label, text or "")
    end)
end

local function set_result_state(title, phonetic, text)
    set_word_summary(title, phonetic)
    set_result_text(text)
    if refs.result_card then
        lv.obj_scroll_to_y(refs.result_card, 0, 0)
    end
end

local function show_keyboard(show)
    state.keyboard_visible = show and true or false
    with_batch(function()
        if state.keyboard_visible then
            lv.obj_clear_flag(refs.keyboard, lv.FLAG_HIDDEN)
        else
            lv.obj_add_flag(refs.keyboard, lv.FLAG_HIDDEN)
        end
    end)
end

local function estimate_history_width(text)
    local content = trim(text or "")
    local width = 18 + (#content * 8)
    if width < 44 then
        width = 44
    end
    if width > 220 then
        width = 220
    end
    return width
end

local function remember_word(word)
    local normalized = trim(string.lower(word or ""))
    if normalized == "" then
        return
    end

    local next_history = { normalized }
    for _, item in ipairs(state.history) do
        if item ~= normalized and #next_history < history_limit then
            next_history[#next_history + 1] = item
        end
    end
    state.history = next_history

    with_batch(function()
        for i, btn in ipairs(refs.history_buttons) do
            local label = refs.history_labels[i]
            local value = state.history[i]
            if value and value ~= "" then
                lv.label_set_text(label, value)
                lv.obj_set_width(btn, estimate_history_width(value))
                lv.obj_clear_flag(btn, lv.FLAG_HIDDEN)
            else
                lv.label_set_text(label, "")
                lv.obj_set_width(btn, 44)
                lv.obj_add_flag(btn, lv.FLAG_HIDDEN)
            end
        end
    end)
end

local function add_section(lines, title, items)
    if not items or #items == 0 then
        return
    end
    if #lines > 0 then
        lines[#lines + 1] = ""
    end
    lines[#lines + 1] = string.format("#%s %s#", SECTION_COLOR, title)
    for _, item in ipairs(items) do
        lines[#lines + 1] = item
    end
end

local function format_translations(data)
    local rows = {}
    for _, item in ipairs(data.translations or {}) do
        local pos = trim(item.pos or "")
        local tran = trim(item.tran_cn or item.tran or "")
        if tran ~= "" then
            rows[#rows + 1] = string.format("%d. %s", #rows + 1, join_parts({ pos, tran }, " "))
        end
        if #rows >= 4 then
            break
        end
    end
    return rows
end

local function format_phrases(data)
    local rows = {}
    for _, item in ipairs(data.phrases or {}) do
        local phrase = trim(item.p_content or "")
        local meaning = trim(item.p_cn or "")
        if phrase ~= "" or meaning ~= "" then
            if phrase ~= "" and meaning ~= "" then
                rows[#rows + 1] = string.format("- %s - %s", phrase, meaning)
            elseif phrase ~= "" then
                rows[#rows + 1] = "- " .. phrase
            else
                rows[#rows + 1] = "- " .. meaning
            end
        end
        if #rows >= 4 then
            break
        end
    end
    return rows
end

local function format_sentences(data)
    local rows = {}
    for _, item in ipairs(data.sentences or {}) do
        local english = trim(item.s_content or "")
        local chinese = trim(item.s_cn or "")
        if english ~= "" then
            rows[#rows + 1] = string.format("%d. %s", #rows + 1, english)
            if chinese ~= "" then
                rows[#rows + 1] = "   " .. chinese
            end
        end
        if #rows >= 6 then
            break
        end
    end
    return rows
end

local function format_synonyms(data)
    local rows = {}
    local seen = {}
    for _, group in ipairs(data.synonyms or {}) do
        for _, item in ipairs(group.Hwds or {}) do
            local word = trim(item.word or item.hwd or "")
            local tran = trim(group.tran or item.tran or "")
            if word ~= "" and not seen[word] then
                seen[word] = true
                rows[#rows + 1] = string.format("- %s%s", word, tran ~= "" and (" - " .. tran) or "")
            end
            if #rows >= 4 then
                return rows
            end
        end
    end
    return rows
end

local function format_related_words(data)
    local rows = {}
    local seen = {}
    for _, group in ipairs(data.relWords or {}) do
        local pos = trim(group.Pos or group.pos or "")
        for _, item in ipairs(group.Hwds or {}) do
            local word = trim(item.hwd or item.word or "")
            local tran = trim(item.tran or "")
            if word ~= "" and not seen[word] then
                seen[word] = true
                local prefix = pos ~= "" and ("[" .. pos .. "] ") or ""
                rows[#rows + 1] = string.format("- %s%s%s", prefix, word, tran ~= "" and (" - " .. tran) or "")
            end
            if #rows >= 6 then
                return rows
            end
        end
    end
    return rows
end

local function build_result_text(data)
    local lines = {}

    add_section(lines, "释义", format_translations(data))
    add_section(lines, "短语", format_phrases(data))
    add_section(lines, "例句", format_sentences(data))
    add_section(lines, "近义词", format_synonyms(data))
    add_section(lines, "相关词", format_related_words(data))

    if #lines == 0 then
        return "接口已返回数据，但没有可展示的释义内容。"
    end
    return table.concat(lines, "\n")
end

local function build_phonetic_text(data)
    local parts = {}
    if data.ukphone and data.ukphone ~= "" then
        parts[#parts + 1] = "英 /" .. data.ukphone .. "/"
    end
    if data.usphone and data.usphone ~= "" then
        parts[#parts + 1] = "美 /" .. data.usphone .. "/"
    end
    if #parts == 0 then
        return "未提供音标"
    end
    return table.concat(parts, "    ")
end

local function render_error(title, message)
    set_result_state(title or "查询失败", "请检查网络或接口返回", message or "未知错误")
end

local function query_word(word)
    local normalized = trim(string.lower(word or ""))
    if normalized == "" then
        ui.show_notification("请输入英文单词", "dict", 1500)
        return
    end

    remember_word(normalized)
    lv.textarea_set_text(refs.input_box, normalized)
    show_keyboard(false)
    set_result_state(normalized, "正在请求词典接口...", "请稍候，正在获取释义和例句。")
    lv.poll_events(10)

    local ok, code, _, body = pcall(function()
        local status, headers, response = http.request("GET", API_BASE .. urlencode(normalized), nil, nil, {
            timeout = 12000
        })
        return status, headers, response
    end)

    if not ok then
        render_error(normalized, "HTTP 请求异常: " .. tostring(code))
        return
    end

    if code ~= 200 or type(body) ~= "string" or body == "" then
        render_error(normalized, "接口返回异常，状态码: " .. tostring(code))
        return
    end

    local parsed_ok, payload = pcall(json.decode, body)
    if not parsed_ok or type(payload) ~= "table" then
        render_error(normalized, "JSON 解析失败，请稍后重试。")
        return
    end

    if tonumber(payload.code or 0) ~= 200 or type(payload.data) ~= "table" then
        local error_text = trim(payload.msg or "")
        if error_text == "" then
            error_text = "接口没有返回有效的单词数据。"
        end
        render_error(normalized, error_text)
        return
    end

    local data = payload.data
    local title = trim(data.word or normalized)
    local phonetic = build_phonetic_text(data)
    local result_text = build_result_text(data)
    set_result_state(title, phonetic, result_text)
end

local function make_button(parent, text, width, height, bg_color, callback)
    local btn = lv.btn_create(parent)
    lv.obj_set_size(btn, width, height)
    lv.obj_set_style_bg_color(btn, bg_color, 0)
    lv.obj_set_style_border_width(btn, 0, 0)
    lv.obj_set_style_radius(btn, 12, 0)

    local label = lv.label_create(btn)
    lv.label_set_text(label, text)
    lv.obj_center(label)

    if callback then
        lv.obj_add_event_cb(btn, callback, lv.EVENT_CLICKED)
    end

    return btn, label
end

local function keyboard_btn_text(kb)
    local btn_id = lv.btnmatrix_get_selected_btn(kb)
    if not btn_id then
        return ""
    end
    local text = lv.btnmatrix_get_btn_text(kb, btn_id)
    return trim(text or "")
end

local function is_keyboard_close(text)
    return text == KEYBOARD_CLOSE
        or text == KEYBOARD_SYMBOL
        or text == "Close"
        or text == "close"
        or text == "收起"
end

local function is_keyboard_ok(text)
    return text == KEYBOARD_OK
        or text == "OK"
        or text == "Ok"
        or text == "ok"
        or text == "确认"
        or text == "完成"
        or text == "Search"
end

refs.input_box = lv.textarea_create(page)
lv.obj_set_size(refs.input_box, 196, 34)
lv.obj_align(refs.input_box, lv.ALIGN_TOP_LEFT, 12, 34)
lv.textarea_set_one_line(refs.input_box, true)
lv.textarea_set_cursor_click_pos(refs.input_box, true)
lv.textarea_set_placeholder_text(refs.input_box, "输入单词，例如 seven")

local search_btn, search_label = make_button(page, "查询", 94, 34, 0x1B54A6, function()
    query_word(lv.textarea_get_text(refs.input_box))
end)
lv.obj_align(search_btn, lv.ALIGN_TOP_RIGHT, -12, 34)
lv.obj_set_style_text_color(search_btn, 0xFFFFFF, 0)

local history_panel = lv.obj_create(page)
lv.obj_set_size(history_panel, 296, 26)
lv.obj_align(history_panel, lv.ALIGN_TOP_LEFT, 12, 74)
lv.obj_set_style_bg_opa(history_panel, lv.OPA_TRANSP, 0)
lv.obj_set_style_border_width(history_panel, 0, 0)
lv.obj_set_style_pad_all(history_panel, 0, 0)
lv.obj_set_style_pad_column(history_panel, 4, 0)
lv.obj_set_scroll_dir(history_panel, lv.DIR_HOR)
lv.obj_set_flex_flow(history_panel, lv.FLEX_FLOW_ROW)
lv.obj_set_flex_align(history_panel, lv.FLEX_ALIGN_START, lv.FLEX_ALIGN_CENTER, lv.FLEX_ALIGN_CENTER)

refs.history_buttons = {}
refs.history_labels = {}
for i = 1, history_limit do
    local btn, label = make_button(history_panel, state.history[i] or "", 44, 24, 0xDCE8F7, function(e)
        local target = lv.event_get_target(e)
        local child = lv.obj_get_child(target, 0)
        local word = trim(lv.label_get_text(child))
        if word ~= "" then
            query_word(word)
        end
    end)
    lv.obj_set_style_text_color(btn, 0x1B54A6, 0)
    if trim(state.history[i] or "") == "" then
        lv.obj_add_flag(btn, lv.FLAG_HIDDEN)
    end
    refs.history_buttons[i] = btn
    refs.history_labels[i] = label
end

local result_card = lv.obj_create(page)
refs.result_card = result_card
lv.obj_set_size(result_card, 296, 120)
lv.obj_align(result_card, lv.ALIGN_TOP_MID, 0, 106)
lv.obj_set_style_bg_color(result_card, 0xFFFFFF, 0)
lv.obj_set_style_border_width(result_card, 0, 0)
lv.obj_set_style_radius(result_card, 14, 0)
lv.obj_set_style_pad_top(result_card, 10, 0)
lv.obj_set_style_pad_bottom(result_card, 10, 0)
lv.obj_set_style_pad_left(result_card, 10, 0)
lv.obj_set_style_pad_right(result_card, 10, 0)

refs.word_label = lv.label_create(result_card)
lv.obj_set_width(refs.word_label, 272)
lv.obj_set_style_text_color(refs.word_label, 0x1B54A6, 0)
lv.obj_align(refs.word_label, lv.ALIGN_TOP_LEFT, 0, 0)

refs.phonetic_label = lv.label_create(result_card)
lv.obj_set_width(refs.phonetic_label, 272)
lv.obj_set_style_text_color(refs.phonetic_label, 0x667085, 0)
lv.obj_align(refs.phonetic_label, lv.ALIGN_TOP_LEFT, 0, 22)
lv.obj_set_style_text_font(refs.phonetic_label, ipa_font, 0)

refs.result_label = lv.label_create(result_card)
lv.obj_set_width(refs.result_label, 272)
lv.label_set_recolor(refs.result_label, true)
lv.obj_set_style_text_color(refs.result_label, 0x1F2937, 0)
lv.obj_align(refs.result_label, lv.ALIGN_TOP_LEFT, 0, 44)

refs.keyboard = lv.keyboard_create(page)
lv.keyboard_set_textarea(refs.keyboard, refs.input_box)
lv.obj_set_size(refs.keyboard, 320, 128)
lv.obj_align(refs.keyboard, lv.ALIGN_BOTTOM_MID, 0, 0)
lv.obj_add_flag(refs.keyboard, lv.FLAG_HIDDEN)

lv.obj_add_event_cb(refs.input_box, function()
    show_keyboard(true)
end, lv.EVENT_FOCUSED)

lv.obj_add_event_cb(refs.keyboard, function(e)
    if lv.event_get_code(e) ~= lv.EVENT_VALUE_CHANGED then
        return
    end

    local target = lv.event_get_target(e)
    local text = keyboard_btn_text(target)
    if text == "" then
        return
    end

    if is_keyboard_close(text) then
        show_keyboard(false)
        return
    end

    if is_keyboard_ok(text) then
        query_word(lv.textarea_get_text(refs.input_box))
    end
end, lv.EVENT_VALUE_CHANGED)

lv.obj_add_event_cb(page, function(e)
    if lv.event_get_code(e) == lv.EVENT_SCREEN_UNLOAD_START then
        show_keyboard(false)
        collectgarbage("collect")
    end
end, lv.EVENT_SCREEN_UNLOAD_START)

set_result_state("英语词典", "支持音标、释义、短语、例句和近义词", "点击下方历史词，或在上方输入框中输入单词后查询。")

sys.timerLoopStart(function()
    lv.poll_events(10)
end, 16)

sys.run()
