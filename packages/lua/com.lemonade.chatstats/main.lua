local sys = require("sys")
local lv = lvgl
local http = require("http")
local json = require("json")

local page = lv.app_page()
if not page then return end

lv.obj_clean(page)
lv.obj_set_style_bg_color(page, 0xF6F8FB, 0)
lv.obj_clear_flag(page, lv.FLAG_SCROLLABLE)

local API_BASE = "https://iot.cookie987.top"
local COLOR_PRIMARY = 0x2563EB
local COLOR_PRIMARY_DARK = 0x1D4ED8
local COLOR_TEXT = 0x111827
local COLOR_MUTED = 0x6B7280
local COLOR_CARD = 0xFFFFFF
local COLOR_SOFT = 0xEAF1FF
local COLOR_BORDER = 0xD8E2F0
local KEYBOARD_OK = "\239\128\140"
local KEYBOARD_CLOSE = "\239\128\141"
local KEYBOARD_SYMBOL = "\239\132\156"

local refs = {}
local state = {
    tab = "summary",
    target = "",
    summary = nil,
    badges = nil,
    affinity = nil,
    loading = false,
    keyboard_visible = false,
    affinity_detail = false,
}

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
    if not text then return "" end
    return (tostring(text):gsub("^%s+", ""):gsub("%s+$", ""))
end

local function bool_text(value)
    return value and "是" or "否"
end

local function n(value, default)
    if type(value) == "number" then return value end
    local parsed = tonumber(value)
    if parsed then return parsed end
    return default or 0
end

local function safe_text(value, fallback)
    local text = trim(value)
    if text == "" then return fallback or "-" end
    return text
end

local function ensure_list(value)
    if type(value) == "table" then return value end
    return {}
end

local function urlencode(text)
    return (tostring(text or ""):gsub("([^%w%-_%.~])", function(ch)
        return string.format("%%%02X", string.byte(ch))
    end))
end

local function truncate(text, max_len)
    text = safe_text(text, "")
    if #text <= max_len then return text end
    return text:sub(1, max_len - 3) .. "..."
end

local function format_count(value)
    return _G.tostring(n(value))
end

local function score_icon(score)
    score = n(score)
    if score >= 100 then return "心" end
    if score >= 85 then return "星" end
    if score >= 65 then return "友" end
    if score >= 40 then return "火" end
    return "新"
end

local function relation_comment(score, title, balance)
    score = n(score)
    if score >= 100 then
        return "默契已经满格，像每天都会自然想起的人。"
    elseif score >= 85 then
        return "关系很热络，聊天节奏稳定又舒服。"
    elseif score >= 65 then
        return "熟悉感正在累积，多聊几次就会更靠近。"
    elseif score >= 40 then
        return "已经建立联系，可以主动续一下火花。"
    end
    local hint = safe_text(balance, safe_text(title, ""))
    if hint ~= "" and hint ~= "-" then
        return hint .. "，还需要更多互动来升温。"
    end
    return "刚刚开始认识，先从一句问候慢慢升温。"
end

local function get_device_mac()
    if esp and esp.mac then
        local ok, mac = pcall(esp.mac)
        if ok and trim(mac) ~= "" then return trim(mac) end
    end
    return ""
end

local function get_device_uid()
    if lemonade and lemonade.uid then
        local ok, uid = pcall(lemonade.uid)
        if ok and trim(uid) ~= "" then return trim(uid) end
    end
    return ""
end

local function headers()
    return {
        ["x-device-mac"] = get_device_mac(),
        ["x-device-username"] = get_device_uid(),
    }
end

local function set_body(text)
    with_batch(function()
        lv.obj_clean(refs.body_card)
        local label = lv.label_create(refs.body_card)
        lv.obj_set_width(label, 272)
        lv.obj_set_style_text_color(label, COLOR_MUTED, 0)
        lv.label_set_text(label, text or "")
        lv.obj_align(label, lv.ALIGN_TOP_LEFT, 0, 0)
        lv.obj_scroll_to_y(refs.body_card, 0, 0)
    end)
end

local function set_loading(loading, message)
    state.loading = loading and true or false
end

local function request_json(path)
    local uid = get_device_uid()
    local mac = get_device_mac()
    if uid == "" or uid == "0000" then
        return nil, "设备还没有绑定用户名"
    end
    if mac == "" then
        return nil, "无法读取设备 MAC"
    end

    local ok, code, _, body = pcall(function()
        local status, response_headers, response = http.request("GET", API_BASE .. path, headers(), nil, {
            timeout = 12000
        })
        return status, response_headers, response
    end)
    if not ok then
        return nil, "HTTP 请求异常: " .. tostring(code)
    end
    if code ~= 200 or type(body) ~= "string" or body == "" then
        return nil, "接口返回异常，状态码: " .. tostring(code)
    end

    local parsed_ok, payload = pcall(json.decode, body)
    if not parsed_ok or type(payload) ~= "table" then
        return nil, "JSON 解析失败"
    end
    return payload, nil
end

local function make_button(parent, text, width, height, bg, fg, cb)
    local btn = lv.btn_create(parent)
    lv.obj_set_size(btn, width, height)
    lv.obj_set_style_bg_color(btn, bg, 0)
    lv.obj_set_style_border_width(btn, 0, 0)
    lv.obj_set_style_shadow_width(btn, 0, 0)
    lv.obj_set_style_radius(btn, 8, 0)
    lv.obj_set_style_pad_all(btn, 0, 0)

    local label = lv.label_create(btn)
    lv.label_set_text(label, text)
    lv.obj_set_style_text_color(label, fg, 0)
    lv.obj_center(label)

    if cb then
        lv.obj_add_event_cb(btn, cb, lv.EVENT_CLICKED)
    end
    return btn, label
end

local function clear_body()
    lv.obj_clean(refs.body_card)
    lv.obj_scroll_to_y(refs.body_card, 0, 0)
end

local function add_label(parent, text, x, y, width, color)
    local label = lv.label_create(parent)
    lv.obj_set_width(label, width)
    lv.obj_set_style_text_color(label, color or COLOR_TEXT, 0)
    lv.label_set_text(label, text or "")
    lv.obj_align(label, lv.ALIGN_TOP_LEFT, x, y)
    return label
end

local function add_single_line_label(parent, text, x, y, width, color)
    local label = add_label(parent, text, x, y, width, color)
    if lv.label_set_long_mode and lv.LABEL_LONG_DOT then
        lv.label_set_long_mode(label, lv.LABEL_LONG_DOT)
    end
    return label
end

local function add_block(y, height, bg, border)
    local block = lv.obj_create(refs.body_card)
    lv.obj_set_size(block, 272, height)
    lv.obj_align(block, lv.ALIGN_TOP_LEFT, 0, y)
    lv.obj_set_style_bg_color(block, bg or 0xFFFFFF, 0)
    lv.obj_set_style_border_color(block, border or COLOR_BORDER, 0)
    lv.obj_set_style_border_width(block, 1, 0)
    lv.obj_set_style_radius(block, 8, 0)
    lv.obj_set_style_pad_all(block, 0, 0)
    lv.obj_clear_flag(block, lv.FLAG_SCROLLABLE)
    return block
end

local function add_empty_state(text)
    clear_body()
    local block = add_block(0, 70, 0xFFFFFF, COLOR_BORDER)
    add_label(block, text, 10, 12, 252, COLOR_MUTED)
end

local function add_metric_block(x, y, width, title, value, color)
    local block = lv.obj_create(refs.body_card)
    lv.obj_set_size(block, width, 46)
    lv.obj_align(block, lv.ALIGN_TOP_LEFT, x, y)
    lv.obj_set_style_bg_color(block, 0xF8FBFF, 0)
    lv.obj_set_style_border_color(block, COLOR_BORDER, 0)
    lv.obj_set_style_border_width(block, 1, 0)
    lv.obj_set_style_radius(block, 8, 0)
    lv.obj_set_style_pad_all(block, 0, 0)
    lv.obj_clear_flag(block, lv.FLAG_SCROLLABLE)
    add_single_line_label(block, title, 8, 6, width - 16, COLOR_MUTED)
    add_single_line_label(block, value, 8, 24, width - 16, color or COLOR_TEXT)
    return block
end

local function add_progress(parent, x, y, width, score)
    local bar = lv.bar_create(parent)
    lv.obj_set_size(bar, width, 8)
    lv.obj_align(bar, lv.ALIGN_TOP_LEFT, x, y)
    lv.bar_set_range(bar, 0, 100)
    lv.bar_set_value(bar, math.max(0, math.min(100, n(score))), false)
    lv.obj_set_style_bg_color(bar, 0xDBEAFE, 0)
    lv.obj_set_style_bg_color(bar, COLOR_PRIMARY, lv.PART_INDICATOR)
    lv.obj_set_style_border_width(bar, 0, 0)
    lv.obj_set_style_radius(bar, 4, 0)
    lv.obj_set_style_radius(bar, 4, lv.PART_INDICATOR)
    return bar
end

local KNOWN_BADGES = {
    {
        code = "social_first_step",
        name = "社交起步",
        description = "发送过至少 1 条私聊消息",
    },
    {
        code = "chat_splash_master",
        name = "水花制造者",
        description = "累计参与私聊达到 100 条",
    },
    {
        code = "contact_keeper",
        name = "联系人收藏家",
        description = "联系人数量达到 5 个",
    },
}

local function normalize_badges(payload_badges)
    local by_code = {}
    local by_name = {}
    for _, badge in ipairs(ensure_list(payload_badges)) do
        if type(badge) == "table" then
            if badge.code then by_code[badge.code] = badge end
            if badge.name then by_name[badge.name] = badge end
        end
    end

    local out = {}
    for _, known in ipairs(KNOWN_BADGES) do
        local remote = by_code[known.code] or by_name[known.name] or {}
        out[#out + 1] = {
            code = known.code,
            name = safe_text(remote.name, known.name),
            description = safe_text(remote.description or remote.condition, known.description),
            unlocked = remote.unlocked and true or false,
        }
    end
    return out
end

local function badge_label(badge)
    if type(badge) == "table" then
        return safe_text(badge.name or badge.code or badge.description, "徽章")
    end
    return safe_text(badge, "徽章")
end

local function badge_names(badges)
    local names = {}
    for _, badge in ipairs(ensure_list(badges)) do
        names[#names + 1] = badge_label(badge)
    end
    return names
end

local function update_tabs()
    local tabs = {
        { key = "summary", btn = refs.summary_tab, label = refs.summary_tab_label },
        { key = "badges", btn = refs.badges_tab, label = refs.badges_tab_label },
        { key = "affinity", btn = refs.affinity_tab, label = refs.affinity_tab_label },
    }
    for _, item in ipairs(tabs) do
        if item.key == state.tab then
            lv.obj_set_style_bg_color(item.btn, COLOR_PRIMARY, 0)
            lv.obj_set_style_text_color(item.label, 0xFFFFFF, 0)
        else
            lv.obj_set_style_bg_color(item.btn, COLOR_SOFT, 0)
            lv.obj_set_style_text_color(item.label, COLOR_PRIMARY_DARK, 0)
        end
    end
end

local function show_tab(tab)
    state.tab = tab
    with_batch(function()
        update_tabs()
        if refs.detail_back then
            lv.obj_add_flag(refs.detail_back, lv.FLAG_HIDDEN)
            lv.obj_add_flag(refs.detail_title, lv.FLAG_HIDDEN)
        end
        if tab == "affinity" and state.affinity_detail then
            lv.obj_add_flag(refs.summary_tab, lv.FLAG_HIDDEN)
            lv.obj_add_flag(refs.badges_tab, lv.FLAG_HIDDEN)
            lv.obj_add_flag(refs.affinity_tab, lv.FLAG_HIDDEN)
            lv.obj_add_flag(refs.target_panel, lv.FLAG_HIDDEN)
            lv.obj_clear_flag(refs.detail_back, lv.FLAG_HIDDEN)
            lv.obj_clear_flag(refs.detail_title, lv.FLAG_HIDDEN)
            lv.label_set_text(refs.detail_title, safe_text(state.target, "好友详情"))
            lv.obj_set_height(refs.body_card, 158)
            lv.obj_align(refs.body_card, lv.ALIGN_TOP_MID, 0, 72)
            lv.obj_clear_flag(refs.body_card, lv.FLAG_HIDDEN)
        elseif tab == "affinity" then
            lv.obj_clear_flag(refs.summary_tab, lv.FLAG_HIDDEN)
            lv.obj_clear_flag(refs.badges_tab, lv.FLAG_HIDDEN)
            lv.obj_clear_flag(refs.affinity_tab, lv.FLAG_HIDDEN)
            lv.obj_clear_flag(refs.target_panel, lv.FLAG_HIDDEN)
            lv.obj_set_height(refs.body_card, 112)
            lv.obj_align(refs.body_card, lv.ALIGN_TOP_MID, 0, 112)
            if state.keyboard_visible then
                lv.obj_add_flag(refs.body_card, lv.FLAG_HIDDEN)
            else
                lv.obj_clear_flag(refs.body_card, lv.FLAG_HIDDEN)
            end
        else
            state.affinity_detail = false
            lv.obj_clear_flag(refs.summary_tab, lv.FLAG_HIDDEN)
            lv.obj_clear_flag(refs.badges_tab, lv.FLAG_HIDDEN)
            lv.obj_clear_flag(refs.affinity_tab, lv.FLAG_HIDDEN)
            lv.obj_add_flag(refs.target_panel, lv.FLAG_HIDDEN)
            lv.obj_clear_flag(refs.body_card, lv.FLAG_HIDDEN)
            lv.obj_set_height(refs.body_card, 158)
            lv.obj_align(refs.body_card, lv.ALIGN_TOP_MID, 0, 72)
        end
    end)
end

local function format_summary(payload)
    local rows = {}
    rows[#rows + 1] = "用户: " .. safe_text(payload and payload.user, get_device_uid())
    rows[#rows + 1] = ""

    local affinities = ensure_list(payload and payload.affinities)
    if #affinities == 0 then
        rows[#rows + 1] = "暂无聊天亲密度数据。"
        rows[#rows + 1] = "可以先和好友聊几句，再回来刷新。"
        return table.concat(rows, "\n")
    end

    rows[#rows + 1] = "好友亲密度"
    for i, item in ipairs(affinities) do
        if i > 8 then
            rows[#rows + 1] = string.format("... 还有 %d 位好友", #affinities - 8)
            break
        end
        rows[#rows + 1] = string.format(
            "%d. %s  Lv.%d  %d分  %s",
            i,
            truncate(item.username or "-", 18),
            n(item.level),
            n(item.score),
            safe_text(item.title, "好友")
        )
        rows[#rows + 1] = string.format("   火花%d天  水花%d", n(item.sparkDays), n(item.chatSplash))
    end
    return table.concat(rows, "\n")
end

local function format_badges(payload)
    local rows = {}
    rows[#rows + 1] = "用户: " .. safe_text(payload and payload.user, get_device_uid())
    rows[#rows + 1] = ""

    local badges = ensure_list(payload and payload.badges)
    if #badges == 0 then
        rows[#rows + 1] = "暂无徽章数据。"
        return table.concat(rows, "\n")
    end

    local unlocked = 0
    for _, badge in ipairs(badges) do
        if badge.unlocked then unlocked = unlocked + 1 end
    end
    rows[#rows + 1] = string.format("已解锁: %d / %d", unlocked, #badges)
    rows[#rows + 1] = ""
    for i, badge in ipairs(badges) do
        if i > 10 then
            rows[#rows + 1] = string.format("... 还有 %d 个徽章", #badges - 10)
            break
        end
        local mark = badge.unlocked and "[已解锁]" or "[未解锁]"
        rows[#rows + 1] = string.format("%s %s", mark, safe_text(badge.name, badge.code or "徽章"))
        local desc = trim(badge.description)
        if desc ~= "" then
            rows[#rows + 1] = "   " .. desc
        end
    end
    return table.concat(rows, "\n")
end

local function format_affinity(payload)
    if type(payload) ~= "table" then
        return "输入目标 UID 后点击查询。"
    end
    local rows = {}
    rows[#rows + 1] = string.format("%s  Lv.%d", safe_text(payload.targetUsername, state.target), n(payload.level))
    rows[#rows + 1] = string.format("%s  %d分", safe_text(payload.title, "好友"), n(payload.score))
    rows[#rows + 1] = ""
    rows[#rows + 1] = string.format("火花天数: %d", n(payload.sparkDays))
    rows[#rows + 1] = string.format("今日活跃: %s", bool_text(payload.isActiveToday))
    rows[#rows + 1] = string.format("聊天水花: %d", n(payload.chatSplash))
    rows[#rows + 1] = string.format("近7日消息: %d", n(payload.messagesLast7Days))
    rows[#rows + 1] = "关系状态: " .. safe_text(payload.balanceTitle, "-")

    local badges = normalize_badges(payload.badges)
    if #badges > 0 then
        rows[#rows + 1] = ""
        rows[#rows + 1] = "相关徽章: " .. table.concat(badge_names(badges), " / ")
    end
    return table.concat(rows, "\n")
end

local function render_summary_cards(payload)
    if type(payload) ~= "table" then
        add_empty_state("点击“总览”刷新聊天统计。")
        return
    end

    clear_body()
    local affinities = ensure_list(payload.affinities)
    local total_splash = 0
    local active_sparks = 0
    for _, item in ipairs(affinities) do
        total_splash = total_splash + n(item.chatSplash)
        if n(item.sparkDays) > 0 then active_sparks = active_sparks + 1 end
    end

    add_metric_block(0, 0, 86, "好友", format_count(#affinities), COLOR_PRIMARY)
    add_metric_block(90, 0, 86, "火花", format_count(active_sparks), 0xF97316)
    add_metric_block(180, 0, 92, "水花", format_count(total_splash), 0x0891B2)

    local y = 56
    if #affinities == 0 then
        local block = add_block(y, 66, 0xFFFFFF, COLOR_BORDER)
        add_label(block, "暂无亲密度", 10, 10, 252, COLOR_TEXT)
        add_label(block, "先和好友聊几句，再回来刷新。", 10, 34, 252, COLOR_MUTED)
        return
    end

    for i, item in ipairs(affinities) do
        if i > 6 then
            local more = add_block(y, 34, 0xF8FBFF, COLOR_BORDER)
            add_label(more, string.format("还有 %d 位好友", #affinities - 6), 10, 9, 252, COLOR_MUTED)
            break
        end
        local block = add_block(y, 58, 0xFFFFFF, COLOR_BORDER)
        add_single_line_label(block, truncate(item.username or "-", 18), 10, 8, 130, COLOR_TEXT)
        add_single_line_label(block, safe_text(item.title, "好友"), 10, 30, 82, COLOR_MUTED)
        add_single_line_label(block, string.format("Lv.%d", n(item.level)), 150, 8, 46, COLOR_PRIMARY_DARK)
        add_single_line_label(block, string.format("%d分", n(item.score)), 202, 8, 58, COLOR_PRIMARY)
        add_single_line_label(block, string.format("火花%d天  水花%s", n(item.sparkDays), format_count(item.chatSplash)), 140, 30, 122, COLOR_MUTED)
        y = y + 66
    end
end

local function render_badge_cards(payload)
    if type(payload) ~= "table" then
        add_empty_state("点击“徽章”刷新社交徽章。")
        return
    end

    clear_body()
    local badges = ensure_list(payload.badges)
    local unlocked = 0
    for _, badge in ipairs(badges) do
        if badge.unlocked then unlocked = unlocked + 1 end
    end

    local summary = add_block(0, 50, 0xF8FBFF, COLOR_BORDER)
    add_label(summary, "徽章进度", 10, 8, 120, COLOR_TEXT)
    add_label(summary, string.format("%d / %d", unlocked, #badges), 202, 8, 58, COLOR_PRIMARY)
    add_label(summary, unlocked == #badges and "全部点亮" or "继续聊天解锁更多", 10, 30, 250, COLOR_MUTED)

    local y = 58
    if #badges == 0 then
        local block = add_block(y, 54, 0xFFFFFF, COLOR_BORDER)
        add_label(block, "暂无徽章数据", 10, 16, 252, COLOR_MUTED)
        return
    end

    for _, badge in ipairs(badges) do
        local block = add_block(y, 60, badge.unlocked and 0xFFFFFF or 0xF4F6F8, COLOR_BORDER)
        local status = badge.unlocked and "已解锁" or "未解锁"
        local status_color = badge.unlocked and COLOR_PRIMARY or COLOR_MUTED
        add_single_line_label(block, safe_text(badge.name, badge.code or "徽章"), 10, 8, 150, COLOR_TEXT)
        add_single_line_label(block, status, 208, 8, 54, status_color)
        add_single_line_label(block, safe_text(badge.description, ""), 10, 32, 252, COLOR_MUTED)
        y = y + 68
    end
end

local function render_affinity_landing()
    clear_body()
    local block = add_block(0, 110, 0xFFFFFF, COLOR_BORDER)
    add_label(block, "查询联系人", 10, 10, 252, COLOR_TEXT)
    add_label(block, "输入联系人 UID 后，会打开单独的关系详情页。", 10, 34, 252, COLOR_MUTED)
    if state.target ~= "" then
        add_label(block, "上次查询: " .. state.target, 10, 78, 252, COLOR_PRIMARY_DARK)
    end
end

local function render_affinity_cards(payload)
    if type(payload) ~= "table" then
        render_affinity_landing()
        return
    end

    clear_body()
    local score = n(payload.score)
    local hero = add_block(0, 104, 0xF8FBFF, COLOR_BORDER)
    add_single_line_label(hero, score_icon(score), 10, 12, 34, score >= 85 and 0xE11D48 or COLOR_PRIMARY)
    add_single_line_label(hero, safe_text(payload.targetUsername, state.target), 50, 10, 104, COLOR_TEXT)
    add_single_line_label(hero, safe_text(payload.title, "好友"), 50, 34, 104, COLOR_MUTED)
    add_single_line_label(hero, string.format("%d分", score), 202, 10, 58, COLOR_PRIMARY)
    add_single_line_label(hero, string.format("Lv.%d", n(payload.level)), 202, 34, 58, COLOR_PRIMARY_DARK)
    add_progress(hero, 10, 62, 252, score)
    add_label(hero, relation_comment(score, payload.title, payload.balanceTitle), 10, 78, 252, COLOR_MUTED)

    add_metric_block(0, 114, 84, "火花", format_count(n(payload.sparkDays)) .. "天", 0xF97316)
    add_metric_block(94, 114, 84, "水花", format_count(payload.chatSplash), 0x0891B2)
    add_metric_block(188, 114, 84, "近7日", format_count(payload.messagesLast7Days), COLOR_PRIMARY)

    local active = add_block(170, 40, 0xFFFFFF, COLOR_BORDER)
    add_label(active, "今日活跃", 10, 10, 100, COLOR_MUTED)
    add_label(active, bool_text(payload.isActiveToday), 198, 10, 62, payload.isActiveToday and COLOR_PRIMARY or COLOR_MUTED)

    local badges = ensure_list(payload.badges)
    if #badges > 0 then
        local y = 220
        local title = add_block(y, 32, 0xF8FBFF, COLOR_BORDER)
        add_label(title, "相关徽章", 10, 8, 252, COLOR_TEXT)
        y = y + 40
        for _, badge in ipairs(badges) do
            local block = add_block(y, 36, 0xFFFFFF, COLOR_BORDER)
            add_label(block, badge_label(badge), 10, 9, 252, COLOR_MUTED)
            y = y + 44
        end
    end
end

local function render_current()
    if state.tab == "summary" then
        with_batch(function()
            render_summary_cards(state.summary)
        end)
    elseif state.tab == "badges" then
        with_batch(function()
            render_badge_cards(state.badges)
        end)
    else
        with_batch(function()
            if state.affinity_detail then
                render_affinity_cards(state.affinity)
            else
                render_affinity_landing()
            end
        end)
    end
end

local function refresh_summary()
    show_tab("summary")
    set_loading(true, "正在刷新总览...")
    set_body("正在请求 /api/device-social/summary")
    lv.poll_events(10)

    local payload, err = request_json("/api/device-social/summary")
    set_loading(false, err and "刷新失败" or "总览已更新")
    if err then
        set_body(err)
        return
    end
    state.summary = payload
    render_current()
end

local function refresh_badges()
    show_tab("badges")
    set_loading(true, "正在刷新徽章...")
    set_body("正在请求 /api/device-social/badges")
    lv.poll_events(10)

    local payload, err = request_json("/api/device-social/badges")
    set_loading(false, err and "刷新失败" or "徽章已更新")
    if err then
        set_body(err)
        return
    end
    state.badges = payload
    render_current()
end

local function query_affinity()
    local target = trim(lv.textarea_get_text(refs.target_input))
    if target == "" then
        ui.show_notification("请输入目标 UID", "chatstats", 1500)
        return
    end
    state.target = target
    if fskv then
        fskv.set("last_target", target)
    end
    show_tab("affinity")
    set_loading(true, "正在查询好友详情...")
    set_body("正在请求 /api/device-social/affinity/" .. target)
    lv.poll_events(10)

    local payload, err = request_json("/api/device-social/affinity/" .. urlencode(target))
    set_loading(false, err and "查询失败" or "好友详情已更新")
    if err then
        set_body(err)
        return
    end
    state.affinity = payload
    state.affinity_detail = true
    show_tab("affinity")
    render_current()
end

local function keyboard_btn_text(kb)
    local btn_id = lv.btnmatrix_get_selected_btn(kb)
    if not btn_id then return "" end
    return trim(lv.btnmatrix_get_btn_text(kb, btn_id) or "")
end

local function show_keyboard(show)
    state.keyboard_visible = show and true or false
    if show then
        lv.obj_clear_flag(refs.keyboard, lv.FLAG_HIDDEN)
        if state.tab == "affinity" then
            lv.obj_add_flag(refs.body_card, lv.FLAG_HIDDEN)
        end
    else
        lv.obj_add_flag(refs.keyboard, lv.FLAG_HIDDEN)
        if state.tab == "affinity" then
            lv.obj_clear_flag(refs.body_card, lv.FLAG_HIDDEN)
        end
    end
end

local function is_keyboard_close(text)
    return text == KEYBOARD_CLOSE or text == KEYBOARD_SYMBOL or text == "Close" or text == "close" or text == "收起"
end

local function is_keyboard_ok(text)
    return text == KEYBOARD_OK or text == "OK" or text == "Ok" or text == "ok" or text == "确认" or text == "完成" or text == "Search"
end

if fskv then
    fskv.init()
    state.target = trim(fskv.get("last_target") or "")
end

refs.summary_tab, refs.summary_tab_label = make_button(page, "总览", 88, 28, COLOR_PRIMARY, 0xFFFFFF, function()
    refresh_summary()
end)
lv.obj_align(refs.summary_tab, lv.ALIGN_TOP_LEFT, 12, 34)

refs.badges_tab, refs.badges_tab_label = make_button(page, "徽章", 88, 28, COLOR_SOFT, COLOR_PRIMARY_DARK, function()
    refresh_badges()
end)
lv.obj_align(refs.badges_tab, lv.ALIGN_TOP_LEFT, 116, 34)

refs.affinity_tab, refs.affinity_tab_label = make_button(page, "好友", 88, 28, COLOR_SOFT, COLOR_PRIMARY_DARK, function()
    state.affinity_detail = false
    show_tab("affinity")
    render_current()
end)
lv.obj_align(refs.affinity_tab, lv.ALIGN_TOP_LEFT, 220, 34)

refs.detail_back, refs.detail_back_label = make_button(page, "返回", 58, 28, COLOR_SOFT, COLOR_PRIMARY_DARK, function()
    state.affinity_detail = false
    show_tab("affinity")
    render_current()
end)
lv.obj_align(refs.detail_back, lv.ALIGN_TOP_LEFT, 12, 34)
lv.obj_add_flag(refs.detail_back, lv.FLAG_HIDDEN)

refs.detail_title = lv.label_create(page)
lv.obj_set_width(refs.detail_title, 208)
lv.obj_set_style_text_color(refs.detail_title, COLOR_TEXT, 0)
lv.label_set_text(refs.detail_title, "好友详情")
lv.obj_align(refs.detail_title, lv.ALIGN_TOP_LEFT, 82, 40)
lv.obj_add_flag(refs.detail_title, lv.FLAG_HIDDEN)

refs.target_panel = lv.obj_create(page)
lv.obj_set_size(refs.target_panel, 296, 40)
lv.obj_align(refs.target_panel, lv.ALIGN_TOP_LEFT, 12, 68)
lv.obj_set_style_bg_opa(refs.target_panel, lv.OPA_TRANSP, 0)
lv.obj_set_style_border_width(refs.target_panel, 0, 0)
lv.obj_set_style_pad_all(refs.target_panel, 0, 0)
lv.obj_add_flag(refs.target_panel, lv.FLAG_HIDDEN)

refs.target_input = lv.textarea_create(refs.target_panel)
lv.obj_set_size(refs.target_input, 188, 34)
lv.obj_align(refs.target_input, lv.ALIGN_LEFT_MID, 0, 0)
lv.textarea_set_one_line(refs.target_input, true)
lv.textarea_set_placeholder_text(refs.target_input, "目标 UID")
lv.textarea_set_text(refs.target_input, state.target)

local query_btn = make_button(refs.target_panel, "查询", 96, 34, COLOR_PRIMARY, 0xFFFFFF, function()
    show_keyboard(false)
    query_affinity()
end)
lv.obj_align(query_btn, lv.ALIGN_RIGHT_MID, 0, 0)

refs.body_card = lv.obj_create(page)
lv.obj_set_size(refs.body_card, 296, 158)
lv.obj_align(refs.body_card, lv.ALIGN_TOP_MID, 0, 72)
lv.obj_set_style_bg_opa(refs.body_card, lv.OPA_TRANSP, 0)
lv.obj_set_style_border_width(refs.body_card, 0, 0)
lv.obj_set_style_radius(refs.body_card, 0, 0)
lv.obj_set_style_pad_all(refs.body_card, 0, 0)
lv.obj_set_scroll_dir(refs.body_card, lv.DIR_VER)

refs.keyboard = lv.keyboard_create(page)
lv.keyboard_set_textarea(refs.keyboard, refs.target_input)
lv.obj_set_size(refs.keyboard, 320, 128)
lv.obj_align(refs.keyboard, lv.ALIGN_BOTTOM_MID, 0, 0)
lv.obj_add_flag(refs.keyboard, lv.FLAG_HIDDEN)

lv.obj_add_event_cb(refs.target_input, function()
    show_keyboard(true)
end, lv.EVENT_FOCUSED)

lv.obj_add_event_cb(refs.keyboard, function(e)
    if lv.event_get_code(e) ~= lv.EVENT_VALUE_CHANGED then return end
    local text = keyboard_btn_text(lv.event_get_target(e))
    if text == "" then return end
    if is_keyboard_close(text) then
        show_keyboard(false)
    elseif is_keyboard_ok(text) then
        show_keyboard(false)
        query_affinity()
    end
end, lv.EVENT_VALUE_CHANGED)

lv.obj_add_event_cb(page, function(e)
    if lv.event_get_code(e) == lv.EVENT_SCREEN_UNLOAD_START then
        show_keyboard(false)
        collectgarbage("collect")
    end
end, lv.EVENT_SCREEN_UNLOAD_START)

show_tab("summary")
refresh_summary()

sys.timerLoopStart(function()
    lv.poll_events(10)
end, 16)

sys.run()
