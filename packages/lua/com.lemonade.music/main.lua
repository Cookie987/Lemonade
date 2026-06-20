local sys = require("sys")
local lv = lvgl

local page = lv.app_page()
if not page then return end

local PLAYER = "media_player32"
local MUSIC_DIR = "/sdcard/Music"
local MAX_SCAN_DEPTH = 8
local EXTENSIONS = {
    mp3 = true,
    wav = true,
    flac = true,
}

local THEME = {
    bg_top = 0x14182A,
    bg_bottom = 0x090B14,
    card = 0x1D2234,
    card_soft = 0x262C42,
    card_alt = 0x111522,
    accent = 0x7C5CFF,
    accent_soft = 0x9B8CFF,
    text = 0xFFFFFF,
    muted = 0xA4A9BE,
    dim = 0x6D738A,
    line = 0x343B56,
    success = 0x52D6A2,
    shadow = 0x05070C,
}

local tracks = {}
local current_index = 1
local volume = 0.5
local refresh_timer = nil
local rendered_title = nil
local refs = {}
local show_player
local show_list

local function with_batch(fn)
    lv.batch_begin()
    local ok, result = pcall(fn)
    lv.batch_end()
    if not ok then error(result) end
    return result
end

local function basename(path)
    return (path:gsub("^.*[/\\]", ""))
end

local function lower_ext(path)
    local ext = path:match("%.([^%./]+)$")
    return ext and ext:lower() or ""
end

local function is_valid_utf8(text)
    local i = 1
    local len = #text
    while i <= len do
        local b1 = string.byte(text, i)
        if not b1 then return false end
        if b1 < 0x80 then
            i = i + 1
        elseif b1 >= 0xC2 and b1 <= 0xDF then
            local b2 = string.byte(text, i + 1)
            if not b2 or b2 < 0x80 or b2 > 0xBF then return false end
            i = i + 2
        elseif b1 >= 0xE0 and b1 <= 0xEF then
            local b2 = string.byte(text, i + 1)
            local b3 = string.byte(text, i + 2)
            if not b2 or not b3 or b2 < 0x80 or b2 > 0xBF or b3 < 0x80 or b3 > 0xBF then return false end
            if b1 == 0xE0 and b2 < 0xA0 then return false end
            if b1 == 0xED and b2 >= 0xA0 then return false end
            i = i + 3
        elseif b1 >= 0xF0 and b1 <= 0xF4 then
            local b2 = string.byte(text, i + 1)
            local b3 = string.byte(text, i + 2)
            local b4 = string.byte(text, i + 3)
            if not b2 or not b3 or not b4 then return false end
            if b2 < 0x80 or b2 > 0xBF or b3 < 0x80 or b3 > 0xBF or b4 < 0x80 or b4 > 0xBF then return false end
            if b1 == 0xF0 and b2 < 0x90 then return false end
            if b1 == 0xF4 and b2 > 0x8F then return false end
            i = i + 4
        else
            return false
        end
    end
    return true
end

local function display_name(path, index)
    local name = basename(path)
    local ext = lower_ext(path)
    if is_valid_utf8(name) then
        return name
    end
    if ext ~= "" then
        return string.format("Track %02d.%s", index, ext)
    end
    return string.format("Track %02d", index)
end

local function find_track(path)
    for i, track in ipairs(tracks) do
        if track.path == path then
            return i
        end
    end
    return nil
end

local function safe_call(fn, ...)
    local ok, result = pcall(fn, ...)
    if ok then
        return true, result
    end
    return false, tostring(result)
end

local function get_fs()
    if fs and fs.listdir and fs.isdir then
        return fs
    end
    return nil
end

local function listdir(path)
    local api = get_fs()
    if not api then
        return nil, "文件系统接口不可用"
    end
    local entries, err = api.listdir(path)
    if not entries then
        return nil, err or "目录读取失败"
    end
    return entries
end

local function scan_dir(path, depth)
    if depth > MAX_SCAN_DEPTH then
        return
    end

    local entries, err = listdir(path)
    if not entries then
        error(err)
    end

    for _, name in ipairs(entries) do
        local child = path .. "/" .. name
        if fs.isdir(child) then
            scan_dir(child, depth + 1)
        elseif EXTENSIONS[lower_ext(child)] then
            local index = #tracks + 1
            table.insert(tracks, {
                path = child,
                title = display_name(child, index),
                ext = lower_ext(child),
            })
        end
    end
end

local function load_persisted_volume()
    if fskv and fskv.get then
        local ok, value = safe_call(fskv.get, "volume")
        local numeric = tonumber(value)
        if ok and numeric then
            volume = numeric
        end
    end
end

local function persist_last_track(path)
    if fskv and fskv.set then
        safe_call(fskv.set, "last_track", path)
    end
end

local function persist_volume()
    if fskv and fskv.set then
        safe_call(fskv.set, "volume", volume)
    end
end

local function is_active_player_state(state)
    return state == "PLAYING" or state == "PAUSED" or state == "ANNOUNCING"
end

local function set_pad_all(obj, value, selector)
    lv.obj_set_style_pad_top(obj, value, selector)
    lv.obj_set_style_pad_bottom(obj, value, selector)
    lv.obj_set_style_pad_left(obj, value, selector)
    lv.obj_set_style_pad_right(obj, value, selector)
end

local function set_page_background()
    lv.obj_clean(page)
    lv.obj_clear_flag(page, lv.FLAG_SCROLLABLE)
    lv.obj_set_style_bg_opa(page, lv.OPA_COVER, 0)
    lv.obj_set_style_bg_color(page, THEME.bg_top, 0)
    lv.obj_set_style_bg_grad_color(page, THEME.bg_bottom, 0)
    lv.obj_set_style_bg_grad_dir(page, lv.GRAD_DIR_VER, 0)
end

local function clear_refresh_timer()
    if refresh_timer then
        lv.timer_del(refresh_timer)
        refresh_timer = nil
    end
end

local function reset_refs()
    refs = {}
    rendered_title = nil
end

local function make_card(parent, w, h)
    local obj = lv.obj_create(parent)
    if w then lv.obj_set_width(obj, w) end
    if h then lv.obj_set_height(obj, h) end
    lv.obj_set_style_bg_color(obj, THEME.card, 0)
    lv.obj_set_style_bg_opa(obj, lv.OPA_COVER, 0)
    lv.obj_set_style_bg_grad_color(obj, THEME.card_alt, 0)
    lv.obj_set_style_bg_grad_dir(obj, lv.GRAD_DIR_VER, 0)
    lv.obj_set_style_border_width(obj, 1, 0)
    lv.obj_set_style_border_color(obj, THEME.line, 0)
    lv.obj_set_style_radius(obj, 18, 0)
    lv.obj_set_style_shadow_width(obj, 18, 0)
    lv.obj_set_style_shadow_color(obj, THEME.shadow, 0)
    lv.obj_set_style_shadow_opa(obj, 120, 0)
    lv.obj_set_style_shadow_ofs_y(obj, 6, 0)
    return obj
end

local function make_label(parent, text, color, width, align)
    local label = lv.label_create(parent)
    lv.label_set_text(label, text)
    if width then lv.obj_set_width(label, width) end
    if color then lv.obj_set_style_text_color(label, color, 0) end
    if align then lv.obj_set_style_text_align(label, align, 0) end
    return label
end

local function make_pill_button(parent, text, w, h, bg_color, text_color, cb)
    local btn = lv.btn_create(parent)
    if w then lv.obj_set_width(btn, w) end
    if h then lv.obj_set_height(btn, h) end
    lv.obj_set_style_bg_color(btn, bg_color or THEME.card_soft, 0)
    lv.obj_set_style_bg_opa(btn, lv.OPA_COVER, 0)
    lv.obj_set_style_border_width(btn, 0, 0)
    lv.obj_set_style_radius(btn, 16, 0)
    lv.obj_set_style_shadow_width(btn, 0, 0)
    set_pad_all(btn, 0, 0)
    local label = make_label(btn, text, text_color or THEME.text)
    lv.obj_center(label)
    if cb then
        lv.obj_add_event_cb(btn, function()
            cb()
        end, lv.EVENT_CLICKED)
    end
    return btn, label
end

local function make_track_button(parent, left_text, right_text, cb)
    local btn = lv.btn_create(parent)
    lv.obj_set_width(btn, 288)
    lv.obj_set_height(btn, 48)
    lv.obj_set_style_bg_color(btn, THEME.card_soft, 0)
    lv.obj_set_style_bg_opa(btn, lv.OPA_COVER, 0)
    lv.obj_set_style_border_width(btn, 0, 0)
    lv.obj_set_style_radius(btn, 16, 0)
    lv.obj_set_style_shadow_width(btn, 0, 0)
    set_pad_all(btn, 0, 0)

    local left = make_label(btn, left_text, THEME.text)
    lv.obj_align(left, lv.ALIGN_LEFT_MID, 14, -8)
    local right = make_label(btn, right_text or "", THEME.accent_soft)
    lv.obj_align(right, lv.ALIGN_LEFT_MID, 14, 10)
    lv.obj_set_style_text_color(right, THEME.muted, 0)

    if cb then
        lv.obj_add_event_cb(btn, function()
            cb()
        end, lv.EVENT_CLICKED)
    end
    return btn, left, right
end

local function set_status(text)
    clear_refresh_timer()
    reset_refs()
    set_page_background()

    local card = make_card(page, 292, 192)
    lv.obj_align(card, lv.ALIGN_CENTER, 0, 0)
    set_pad_all(card, 18, 0)

    local title = make_label(card, "音乐", THEME.text, 248, lv.TEXT_ALIGN_CENTER)
    lv.obj_align(title, lv.ALIGN_TOP_MID, 0, 6)

    local detail = make_label(card, text, THEME.muted, 248, lv.TEXT_ALIGN_CENTER)
    lv.obj_align(detail, lv.ALIGN_CENTER, 0, 18)
end

local function load_tracks()
    tracks = {}
    load_persisted_volume()

    local api = get_fs()
    if not api then
        return false, "文件系统接口不可用"
    end

    local ok, is_dir = safe_call(api.isdir, MUSIC_DIR)
    if not ok then
        return false, "音乐目录读取失败"
    end
    if not is_dir then
        return false, "/sdcard/Music 目录不存在"
    end

    ok, err = pcall(function()
        scan_dir(MUSIC_DIR, 1)
    end)
    if not ok then
        return false, "音乐列表读取失败: " .. tostring(err)
    end

    table.sort(tracks, function(a, b)
        return a.path:lower() < b.path:lower()
    end)

    if #tracks == 0 then
        return false, "/Music 内没有可播放文件"
    end

    local last = nil
    if fskv and fskv.get then
        local fskv_ok, value = safe_call(fskv.get, "last_track")
        if fskv_ok then last = value end
    end
    if not last and media_player and media_player.last_url then
        local last_ok, last_value = safe_call(media_player.last_url, PLAYER)
        if last_ok then last = last_value end
    end
    local found = last and find_track(last)
    if found then current_index = found end
    if media_player and media_player.volume_set then
        safe_call(media_player.volume_set, PLAYER, volume)
    end
    return true
end

local function get_player_state()
    if media_player and media_player.get_state then
        local ok, value = safe_call(media_player.get_state, PLAYER)
        if ok and value then return value end
    end
    return "UNKNOWN"
end

local function get_last_url()
    if fskv and fskv.get then
        local fskv_ok, value = safe_call(fskv.get, "last_track")
        if fskv_ok and value then return value end
    end
    if media_player and media_player.last_url then
        local ok, value = safe_call(media_player.last_url, PLAYER)
        if ok then return value end
    end
    return nil
end

local function update_track_row_styles()
    if not refs.list_rows then return end
    for i, row in ipairs(refs.list_rows) do
        if i == current_index then
            lv.obj_set_style_bg_color(row.button, THEME.accent, 0)
            lv.obj_set_style_bg_grad_color(row.button, THEME.accent_soft, 0)
            lv.obj_set_style_bg_grad_dir(row.button, lv.GRAD_DIR_HOR, 0)
            lv.obj_set_style_text_color(row.title, THEME.text, 0)
            lv.obj_set_style_text_color(row.meta, THEME.text, 0)
        else
            lv.obj_set_style_bg_color(row.button, THEME.card_soft, 0)
            lv.obj_set_style_bg_grad_color(row.button, THEME.card_soft, 0)
            lv.obj_set_style_bg_grad_dir(row.button, lv.GRAD_DIR_NONE, 0)
            lv.obj_set_style_text_color(row.title, THEME.text, 0)
            lv.obj_set_style_text_color(row.meta, THEME.muted, 0)
        end
    end
end

local function update_playing_labels()
    if not refs.title_label then return end
    local track = tracks[current_index]
    local state = get_player_state()
    if media_player and media_player.get_volume then
        local ok, value = safe_call(media_player.get_volume, PLAYER)
        local numeric = tonumber(value)
        if ok and numeric then volume = numeric end
    end
    local percent = math.floor(volume * 100 + 0.5)
    local action_text = (state == "PLAYING" or state == "ANNOUNCING") and "暂停" or "播放"

    with_batch(function()
        local next_title = track and track.title or "No track"
        if rendered_title ~= next_title then
            rendered_title = next_title
            lv.label_set_text(refs.title_label, rendered_title)
        end
        if refs.subtitle_label then
            lv.label_set_text(refs.subtitle_label, string.format("%s  ·  %d/%d", string.upper(track and (track.ext or "") or ""), current_index, #tracks))
        end
        if refs.state_label then
            lv.label_set_text(refs.state_label, state)
        end
        if refs.volume_label then
            lv.label_set_text(refs.volume_label, string.format("音量 %d%%", percent))
        end
        if refs.volume_bar then
            lv.bar_set_value(refs.volume_bar, percent, 0)
        end
        if refs.play_button_label then
            lv.label_set_text(refs.play_button_label, action_text)
        end
        if refs.cover_title then
            lv.label_set_text(refs.cover_title, string.format("%02d", current_index))
        end
    end)
    update_track_row_styles()
end

local function play_index(index)
    if #tracks == 0 then return end
    if index < 1 then index = #tracks end
    if index > #tracks then index = 1 end
    current_index = index
    local track = tracks[current_index]
    if media_player and media_player.play then
        safe_call(media_player.play, PLAYER, track.path)
        persist_last_track(track.path)
    end
    update_playing_labels()
end

local function change_volume(delta)
    if media_player and media_player.get_volume then
        local ok, value = safe_call(media_player.get_volume, PLAYER)
        local numeric = tonumber(value)
        if ok and numeric then volume = numeric end
    end
    volume = volume + delta
    if volume < 0 then volume = 0 end
    if volume > 1 then volume = 1 end
    if media_player and media_player.volume_set then
        safe_call(media_player.volume_set, PLAYER, volume)
    end
    persist_volume()
    update_playing_labels()
end

local function toggle_pause()
    local state = get_player_state()
    if state == "PAUSED" then
        if media_player and media_player.play then
            local last = get_last_url()
            if last then safe_call(media_player.play, PLAYER, last) end
        end
    elseif state == "PLAYING" or state == "ANNOUNCING" then
        if media_player and media_player.pause then safe_call(media_player.pause, PLAYER) end
    else
        play_index(current_index)
    end
    update_playing_labels()
end

show_list = function()
    clear_refresh_timer()
    reset_refs()
    set_page_background()

    local shell = lv.obj_create(page)
    lv.obj_set_size(shell, 306, 232)
    lv.obj_align(shell, lv.ALIGN_CENTER, 0, 0)
    lv.obj_set_style_bg_opa(shell, 0, 0)
    lv.obj_set_style_border_width(shell, 0, 0)
    set_pad_all(shell, 0, 0)

    local header = lv.obj_create(shell)
    lv.obj_set_size(header, 306, 34)
    lv.obj_align(header, lv.ALIGN_TOP_MID, 0, 0)
    lv.obj_set_style_bg_opa(header, 0, 0)
    lv.obj_set_style_border_width(header, 0, 0)
    set_pad_all(header, 0, 0)

    local eyebrow = make_label(header, "ALL TRACKS", THEME.accent_soft)
    lv.obj_align(eyebrow, lv.ALIGN_TOP_LEFT, 4, 0)
    local title = make_label(header, "音乐库", THEME.text)
    lv.obj_align(title, lv.ALIGN_BOTTOM_LEFT, 4, 2)

    local refresh_btn = make_pill_button(header, "刷新", 68, 28, THEME.card_soft, THEME.text, function()
        local ok, err = load_tracks()
        if ok then show_list() else set_status(err) end
    end)
    lv.obj_align(refresh_btn, lv.ALIGN_RIGHT_MID, 0, 4)

    local list_card = make_card(shell, 306, 164)
    lv.obj_align(list_card, lv.ALIGN_TOP_MID, 0, 42)
    lv.obj_set_style_bg_color(list_card, THEME.card_alt, 0)
    lv.obj_set_style_bg_grad_color(list_card, THEME.card, 0)
    set_pad_all(list_card, 10, 0)

    local list = lv.obj_create(list_card)
    lv.obj_set_size(list, 284, 142)
    lv.obj_align(list, lv.ALIGN_CENTER, 0, 0)
    lv.obj_set_style_bg_opa(list, 0, 0)
    lv.obj_set_style_border_width(list, 0, 0)
    lv.obj_set_style_pad_row(list, 8, 0)
    lv.obj_set_scroll_dir(list, lv.DIR_VER)
    lv.obj_set_flex_flow(list, lv.FLEX_FLOW_COLUMN)
    lv.obj_set_flex_align(list, lv.FLEX_ALIGN_START, lv.FLEX_ALIGN_CENTER, lv.FLEX_ALIGN_CENTER)
    set_pad_all(list, 0, 0)

    refs.list_rows = {}
    for i, track in ipairs(tracks) do
        local meta = string.format("%s · #%d", string.upper(track.ext or "FILE"), i)
        local button, title_label, meta_label = make_track_button(list, track.title, meta, function()
            play_index(i)
            show_player()
        end)
        refs.list_rows[#refs.list_rows + 1] = {
            button = button,
            title = title_label,
            meta = meta_label,
        }
    end

    local footer_btn = make_pill_button(shell, "正在播放", 110, 28, THEME.card_soft, THEME.text, function()
        show_player()
    end)
    lv.obj_align(footer_btn, lv.ALIGN_BOTTOM_MID, 0, 0)

    update_track_row_styles()
end

show_player = function()
    if not media_player then
        set_status("播放器接口不可用")
        return
    end

    clear_refresh_timer()
    reset_refs()
    set_page_background()

    local shell = lv.obj_create(page)
    lv.obj_set_size(shell, 306, 232)
    lv.obj_align(shell, lv.ALIGN_CENTER, 0, 0)
    lv.obj_set_style_bg_opa(shell, 0, 0)
    lv.obj_set_style_border_width(shell, 0, 0)
    set_pad_all(shell, 0, 0)

    local top = lv.obj_create(shell)
    lv.obj_set_size(top, 306, 26)
    lv.obj_align(top, lv.ALIGN_TOP_MID, 0, 0)
    lv.obj_set_style_bg_opa(top, 0, 0)
    lv.obj_set_style_border_width(top, 0, 0)
    set_pad_all(top, 0, 0)

    local top_left = make_label(top, "NOW PLAYING", THEME.accent_soft)
    lv.obj_align(top_left, lv.ALIGN_LEFT_MID, 4, 0)
    local top_right = make_label(top, string.format("%d 首", #tracks), THEME.dim)
    lv.obj_align(top_right, lv.ALIGN_RIGHT_MID, -4, 0)

    local panel = make_card(shell, 306, 194)
    lv.obj_align(panel, lv.ALIGN_BOTTOM_MID, 0, 0)
    set_pad_all(panel, 14, 0)

    local cover = make_card(panel, 108, 108)
    lv.obj_align(cover, lv.ALIGN_TOP_LEFT, 0, 0)
    lv.obj_set_style_bg_color(cover, THEME.accent, 0)
    lv.obj_set_style_bg_grad_color(cover, THEME.card_soft, 0)
    lv.obj_set_style_bg_grad_dir(cover, lv.GRAD_DIR_VER, 0)
    lv.obj_set_style_border_width(cover, 0, 0)
    lv.obj_set_style_radius(cover, 28, 0)
    lv.obj_set_style_shadow_width(cover, 26, 0)
    lv.obj_set_style_shadow_opa(cover, 140, 0)

    refs.cover_title = make_label(cover, "01", THEME.text)
    lv.obj_set_style_text_color(refs.cover_title, THEME.text, 0)
    lv.obj_set_style_text_opa(refs.cover_title, 210, 0)
    lv.obj_align(refs.cover_title, lv.ALIGN_CENTER, 0, -8)
    local cover_sub = make_label(cover, "MUSIC", 0xE6E0FF)
    lv.obj_align(cover_sub, lv.ALIGN_CENTER, 0, 18)

    refs.title_label = make_label(panel, "", THEME.text, 152)
    if lv.LABEL_LONG_MODE_CLIP then
        lv.label_set_long_mode(refs.title_label, lv.LABEL_LONG_MODE_CLIP)
    end
    lv.obj_align(refs.title_label, lv.ALIGN_TOP_RIGHT, 0, 6)

    refs.subtitle_label = make_label(panel, "", THEME.muted, 152)
    lv.obj_align(refs.subtitle_label, lv.ALIGN_TOP_RIGHT, 0, 42)

    refs.state_label = make_label(panel, "", THEME.accent_soft, 152)
    lv.obj_align(refs.state_label, lv.ALIGN_TOP_RIGHT, 0, 70)

    local volume_card = lv.obj_create(panel)
    lv.obj_set_size(volume_card, 278, 34)
    lv.obj_align(volume_card, lv.ALIGN_TOP_MID, 0, 120)
    lv.obj_set_style_bg_color(volume_card, THEME.card_soft, 0)
    lv.obj_set_style_bg_opa(volume_card, lv.OPA_COVER, 0)
    lv.obj_set_style_border_width(volume_card, 0, 0)
    lv.obj_set_style_radius(volume_card, 14, 0)
    set_pad_all(volume_card, 0, 0)

    refs.volume_label = make_label(volume_card, "", THEME.text)
    lv.obj_align(refs.volume_label, lv.ALIGN_LEFT_MID, 12, 0)

    refs.volume_bar = lv.bar_create(volume_card)
    lv.obj_set_size(refs.volume_bar, 110, 8)
    lv.obj_align(refs.volume_bar, lv.ALIGN_RIGHT_MID, -12, 0)
    lv.bar_set_range(refs.volume_bar, 0, 100)
    lv.obj_set_style_bg_color(refs.volume_bar, THEME.line, 0)
    lv.obj_set_style_bg_opa(refs.volume_bar, lv.OPA_COVER, 0)
    lv.obj_set_style_radius(refs.volume_bar, 8, 0)
    lv.obj_set_style_bg_color(refs.volume_bar, THEME.accent, lv.PART_INDICATOR)
    lv.obj_set_style_bg_grad_color(refs.volume_bar, THEME.accent_soft, lv.PART_INDICATOR)
    lv.obj_set_style_bg_grad_dir(refs.volume_bar, lv.GRAD_DIR_HOR, lv.PART_INDICATOR)

    local controls = lv.obj_create(panel)
    lv.obj_set_size(controls, 278, 44)
    lv.obj_align(controls, lv.ALIGN_BOTTOM_MID, 0, -38)
    lv.obj_set_style_bg_opa(controls, 0, 0)
    lv.obj_set_style_border_width(controls, 0, 0)
    lv.obj_set_flex_flow(controls, lv.FLEX_FLOW_ROW)
    lv.obj_set_flex_align(controls, lv.FLEX_ALIGN_SPACE_BETWEEN, lv.FLEX_ALIGN_CENTER, lv.FLEX_ALIGN_CENTER)
    set_pad_all(controls, 0, 0)

    make_pill_button(controls, "上一首", 82, 42, THEME.card_soft, THEME.text, function()
        play_index(current_index - 1)
    end)
    local play_btn, play_label = make_pill_button(controls, "播放", 82, 42, THEME.accent, THEME.text, function()
        toggle_pause()
    end)
    refs.play_button = play_btn
    refs.play_button_label = play_label
    make_pill_button(controls, "下一首", 82, 42, THEME.card_soft, THEME.text, function()
        play_index(current_index + 1)
    end)

    local bottom = lv.obj_create(panel)
    lv.obj_set_size(bottom, 278, 30)
    lv.obj_align(bottom, lv.ALIGN_BOTTOM_MID, 0, 0)
    lv.obj_set_style_bg_opa(bottom, 0, 0)
    lv.obj_set_style_border_width(bottom, 0, 0)
    lv.obj_set_flex_flow(bottom, lv.FLEX_FLOW_ROW)
    lv.obj_set_flex_align(bottom, lv.FLEX_ALIGN_SPACE_BETWEEN, lv.FLEX_ALIGN_CENTER, lv.FLEX_ALIGN_CENTER)
    set_pad_all(bottom, 0, 0)

    make_pill_button(bottom, "音量-", 62, 28, THEME.card_soft, THEME.text, function() change_volume(-0.1) end)
    make_pill_button(bottom, "停止", 62, 28, THEME.card_soft, THEME.text, function()
        if media_player.stop then safe_call(media_player.stop, PLAYER) end
        update_playing_labels()
    end)
    make_pill_button(bottom, "音量+", 62, 28, THEME.card_soft, THEME.text, function() change_volume(0.1) end)
    make_pill_button(bottom, "列表", 62, 28, THEME.card_soft, THEME.text, show_list)

    update_playing_labels()
    refresh_timer = lv.timer_create(function()
        update_playing_labels()
    end, 1000)
end

local ok, err = load_tracks()
if ok then
    local state = get_player_state()
    local last = get_last_url()
    if last and is_active_player_state(state) then
        show_player()
    else
        show_list()
    end
else
    set_status(err)
end

while true do
    lv.poll_events(100)
end
