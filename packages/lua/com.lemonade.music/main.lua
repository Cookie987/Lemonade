local sys = require("sys")
local lv = lvgl

local page = lv.app_page()
if not page then return end

local PLAYER = "media_player32"
local MUSIC_DIR = "/sdcard/Music"
local EXTENSIONS = {
    mp3 = true,
    wav = true,
    flac = true,
    m4a = true,
    aac = true,
    ogg = true,
}

local tracks = {}
local current_index = 1
local volume = 0.5
local title_label = nil
local state_label = nil
local volume_label = nil
local volume_bar = nil
local refresh_timer = nil
local rendered_title = nil
local show_player

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

local function is_active_player_state(state)
    return state == "PLAYING" or state == "PAUSED" or state == "ANNOUNCING"
end

local function format_size(size)
    return ""
end

local function sync_volume_from_player()
    if media_player and media_player.get_volume then
        local ok, value = safe_call(media_player.get_volume, PLAYER)
        local numeric = tonumber(value)
        if ok and numeric then volume = numeric end
    end
end

local function set_pad_all(obj, value, value2)
    lv.obj_set_style_pad_top(obj, value, value2)
    lv.obj_set_style_pad_bottom(obj, value, value2)
    lv.obj_set_style_pad_left(obj, value, value2)
    lv.obj_set_style_pad_right(obj, value, value2)
end

local function make_btn(parent, text, w, h, cb)
    local btn = lv.btn_create(parent)
    lv.obj_set_size(btn, w, h)
    local label = lv.label_create(btn)
    lv.label_set_text(label, text)
    lv.obj_align(label, lv.ALIGN_CENTER, 0, 0)
    lv.obj_add_event_cb(btn, function()
        cb()
    end, lv.EVENT_CLICKED)
    return btn
end

local function set_status(text)
    lv.obj_clean(page)
    lv.obj_set_style_bg_color(page, 0x101018, 0)
    local label = lv.label_create(page)
    lv.label_set_text(label, text)
    lv.obj_set_width(label, 280)
    lv.obj_set_style_text_color(label, 0xFFFFFF, 0)
    lv.obj_set_style_text_align(label, lv.TEXT_ALIGN_CENTER, 0)
    lv.obj_align(label, lv.ALIGN_CENTER, 0, 0)
end

local function scan_dir(path, depth)
    if depth < 0 then return true end
    local ok, entries_or_err, err = pcall(fs.listdir, path)
    if not ok then
        return false, tostring(entries_or_err)
    end
    if not entries_or_err then
        return false, tostring(err or "unknown")
    end
    local entries = type(entries_or_err) == "table" and entries_or_err or {}
    table.sort(entries, function(a, b) return a:lower() < b:lower() end)

    for _, name in ipairs(entries) do
        local child = path .. "/" .. name
        local is_dir = false
        local dir_ok, dir_value = safe_call(fs.isdir, child)
        if dir_ok and dir_value then is_dir = true end
        if is_dir then
            local child_ok, child_err = scan_dir(child, depth - 1)
            if not child_ok then return false, child_err end
        elseif EXTENSIONS[lower_ext(name)] then
            local index = #tracks + 1
            table.insert(tracks, {
                path = child,
                title = display_name(child, index),
            })
        end
    end
    return true
end

local function load_tracks()
    tracks = {}

    if not fs or not fs.isdir or not fs.listdir then
        return false, "文件系统接口不可用"
    end

    local ok, available = safe_call(fs.isdir, MUSIC_DIR)
    if not ok then
        return false, "音乐目录读取失败"
    end
    if not available then
        return false, "缺少 /sdcard/Music 目录"
    end

    ok, available = scan_dir(MUSIC_DIR, 8)
    if not ok then
        return false, "音乐列表读取失败"
    end

    table.sort(tracks, function(a, b)
        return a.path:lower() < b.path:lower()
    end)

    if #tracks == 0 then
        return false, "/Music 内没有可播放文件"
    end

    local last = nil
    if fskv and fskv.init and fskv.get then
        safe_call(fskv.init)
        local last_ok, last_value = safe_call(fskv.get, "last_track")
        if last_ok then last = last_value end
        local volume_ok, volume_value = safe_call(fskv.get, "volume")
        local numeric = tonumber(volume_value)
        if volume_ok and numeric then volume = numeric end
    end
    local found = last and find_track(last)
    if found then current_index = found end
    return true
end

local function update_playing_labels()
    if not title_label then return end
    local track = tracks[current_index]
    local state = "UNKNOWN"
    if media_player and media_player.get_state then
        local ok, value = safe_call(media_player.get_state, PLAYER)
        if ok and value then state = value end
    end
    sync_volume_from_player()
    local percent = math.floor(volume * 100 + 0.5)

    with_batch(function()
        local next_title = track and track.title or "No track"
        if rendered_title ~= next_title then
            rendered_title = next_title
            lv.label_set_text(title_label, rendered_title)
        end
        lv.label_set_text(state_label, string.format("%s  %d/%d", state, current_index, #tracks))
        lv.label_set_text(volume_label, string.format("音量 %d%%", percent))
        lv.bar_set_value(volume_bar, percent, 0)
    end)
end

local function play_index(index)
    if #tracks == 0 then return end
    if index < 1 then index = #tracks end
    if index > #tracks then index = 1 end
    current_index = index
    if media_player and media_player.play then
        media_player.play(PLAYER, tracks[current_index].path)
    end
    if fskv and fskv.set then
        safe_call(fskv.set, "last_track", tracks[current_index].path)
    end
    update_playing_labels()
end

local function change_volume(delta)
    sync_volume_from_player()
    volume = volume + delta
    if volume < 0 then volume = 0 end
    if volume > 1 then volume = 1 end
    if media_player and media_player.volume_set then
        media_player.volume_set(PLAYER, volume)
    end
    if fskv and fskv.set then
        safe_call(fskv.set, "volume", volume)
    end
    update_playing_labels()
end

local function show_list()
    if refresh_timer then
        lv.timer_del(refresh_timer)
        refresh_timer = nil
    end

    lv.obj_clean(page)
    lv.obj_set_style_bg_color(page, 0x101018, 0)

    local header = lv.obj_create(page)
    lv.obj_set_size(header, 320, 38)
    lv.obj_align(header, lv.ALIGN_TOP_MID, 0, 0)
    lv.obj_set_style_bg_opa(header, 0, 0)
    lv.obj_set_style_border_width(header, 0, 0)
    set_pad_all(header, 4, 0)

    local title = lv.label_create(header)
    lv.label_set_text(title, "音乐")
    lv.obj_set_style_text_color(title, 0xFFFFFF, 0)
    lv.obj_align(title, lv.ALIGN_LEFT_MID, 8, 0)

    local refresh_btn = make_btn(header, "刷新", 70, 30, function()
        local ok, err = load_tracks()
        if ok then show_list() else set_status(err) end
    end)
    lv.obj_align(refresh_btn, lv.ALIGN_RIGHT_MID, -8, 0)

    local list = lv.list_create(page)
    lv.obj_set_size(list, 306, 194)
    lv.obj_align(list, lv.ALIGN_BOTTOM_MID, 0, -4)
    lv.obj_set_style_bg_color(list, 0x181824, 0)
    lv.obj_set_style_border_width(list, 0, 0)

    for i, track in ipairs(tracks) do
        local text = track.title
        local size = format_size(track.size)
        if size ~= "" then text = text .. "  " .. size end
        local btn = lv.list_add_btn(list, nil, text)
        lv.obj_add_event_cb(btn, function()
            play_index(i)
            show_player()
        end, lv.EVENT_CLICKED)
    end
end

show_player = function()
    if not media_player then
        set_status("播放器接口不可用")
        return
    end
    if refresh_timer then
        lv.timer_del(refresh_timer)
        refresh_timer = nil
    end

    lv.obj_clean(page)
    lv.obj_set_style_bg_color(page, 0x101018, 0)

    local panel = lv.obj_create(page)
    lv.obj_set_size(panel, 306, 226)
    lv.obj_align(panel, lv.ALIGN_CENTER, 0, 0)
    lv.obj_set_style_bg_color(panel, 0x181824, 0)
    lv.obj_set_style_border_width(panel, 0, 0)
    lv.obj_set_style_radius(panel, 6, 0)
    set_pad_all(panel, 10, 0)

    rendered_title = nil
    title_label = lv.label_create(panel)
    lv.obj_set_width(title_label, 284)
    if lv.LABEL_LONG_MODE_CLIP then
        lv.label_set_long_mode(title_label, lv.LABEL_LONG_MODE_CLIP)
    end
    lv.obj_set_style_text_color(title_label, 0xFFFFFF, 0)
    lv.obj_align(title_label, lv.ALIGN_TOP_MID, 0, 8)

    state_label = lv.label_create(panel)
    lv.obj_set_style_text_color(state_label, 0xA0A0B0, 0)
    lv.obj_align(state_label, lv.ALIGN_TOP_MID, 0, 44)

    local controls = lv.obj_create(panel)
    lv.obj_set_size(controls, 284, 54)
    lv.obj_align(controls, lv.ALIGN_TOP_MID, 0, 78)
    lv.obj_set_flex_flow(controls, lv.FLEX_FLOW_ROW)
    lv.obj_set_flex_align(controls, lv.FLEX_ALIGN_SPACE_BETWEEN, lv.FLEX_ALIGN_CENTER, lv.FLEX_ALIGN_CENTER)
    lv.obj_set_style_bg_opa(controls, 0, 0)
    lv.obj_set_style_border_width(controls, 0, 0)
    set_pad_all(controls, 0, 0)

    make_btn(controls, "上一首", 78, 44, function()
        play_index(current_index - 1)
    end)
    make_btn(controls, "暂停", 78, 44, function()
        if media_player.pause then media_player.pause(PLAYER) end
        update_playing_labels()
    end)
    make_btn(controls, "下一首", 78, 44, function()
        play_index(current_index + 1)
    end)

    volume_label = lv.label_create(panel)
    lv.obj_set_style_text_color(volume_label, 0xFFFFFF, 0)
    lv.obj_align(volume_label, lv.ALIGN_TOP_LEFT, 8, 144)

    volume_bar = lv.bar_create(panel)
    lv.obj_set_size(volume_bar, 160, 12)
    lv.obj_align(volume_bar, lv.ALIGN_TOP_LEFT, 96, 149)
    lv.bar_set_range(volume_bar, 0, 100)

    local bottom = lv.obj_create(panel)
    lv.obj_set_size(bottom, 284, 46)
    lv.obj_align(bottom, lv.ALIGN_BOTTOM_MID, 0, -2)
    lv.obj_set_flex_flow(bottom, lv.FLEX_FLOW_ROW)
    lv.obj_set_flex_align(bottom, lv.FLEX_ALIGN_SPACE_BETWEEN, lv.FLEX_ALIGN_CENTER, lv.FLEX_ALIGN_CENTER)
    lv.obj_set_style_bg_opa(bottom, 0, 0)
    lv.obj_set_style_border_width(bottom, 0, 0)
    set_pad_all(bottom, 0, 0)

    make_btn(bottom, "音量-", 66, 38, function() change_volume(-0.1) end)
    make_btn(bottom, "停止", 66, 38, function()
        if media_player.stop then media_player.stop(PLAYER) end
        update_playing_labels()
    end)
    make_btn(bottom, "音量+", 66, 38, function() change_volume(0.1) end)
    make_btn(bottom, "列表", 66, 38, show_list)

    update_playing_labels()
    refresh_timer = lv.timer_create(function()
        update_playing_labels()
    end, 1000)
end

local ok, err = load_tracks()
if ok then
    local state = nil
    local last = nil
    if media_player and media_player.get_state then
        local state_ok, state_value = safe_call(media_player.get_state, PLAYER)
        if state_ok then state = state_value end
    end
    if fskv and fskv.get then
        local last_ok, last_value = safe_call(fskv.get, "last_track")
        if last_ok then last = last_value end
    end
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
