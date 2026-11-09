-- 用假的 LVGL / fs / fskv 跑一遍 main.lua，检查书架、打开、翻页、保存、返回。
package.path = "./lib/?.lua;./host_tests/?.lua;" .. package.path

local next_id = 1
local function new_obj(kind, parent)
    local object = {
        id = next_id,
        kind = kind,
        parent = parent,
        children = {},
        callbacks = {},
        flags = {},
        styles = {},
        text = "",
    }
    next_id = next_id + 1
    if parent then
        parent.children[#parent.children + 1] = object
    end
    return object
end

local lv = {
    ALIGN_CENTER = 1,
    ALIGN_TOP_LEFT = 2,
    ALIGN_TOP_MID = 3,
    ALIGN_TOP_RIGHT = 4,
    ALIGN_LEFT_MID = 5,
    ALIGN_RIGHT_MID = 6,
    ALIGN_BOTTOM_MID = 7,
    OPA_TRANSP = 0,
    OPA_COVER = 255,
    PART_INDICATOR = 1,
    FLAG_CLICKABLE = 1,
    FLAG_HIDDEN = 2,
    EVENT_CLICKED = 10,
    EVENT_SCREEN_UNLOAD_START = 11,
    TEXT_ALIGN_LEFT = 0,
    TEXT_ALIGN_CENTER = 1,
    TEXT_ALIGN_RIGHT = 2,
    LABEL_LONG_MODE_CLIP = 0,
    LABEL_LONG_MODE_DOTS = 1,
    LABEL_LONG_MODE_WRAP = 2,
}

local page = new_obj("page", nil)

function lv.app_page()
    return page
end

function lv.obj_create(parent)
    return new_obj("obj", parent)
end

function lv.label_create(parent)
    return new_obj("label", parent)
end

function lv.btn_create(parent)
    return new_obj("btn", parent)
end

function lv.bar_create(parent)
    return new_obj("bar", parent)
end

function lv.obj_clean(object)
    object.children = {}
end

function lv.obj_set_pos(object, x, y)
    object.x, object.y = x, y
end

function lv.obj_set_size(object, width, height)
    object.w, object.h = width, height
end

function lv.obj_set_width(object, width)
    object.w = width
end

function lv.obj_set_height(object, height)
    object.h = height
end

function lv.obj_center()
end

function lv.obj_add_flag(object, flag)
    object.flags[flag] = true
end

function lv.obj_clear_flag(object, flag)
    object.flags[flag] = nil
end

function lv.obj_add_event_cb(object, callback, code)
    object.callbacks[code] = callback
end

function lv.label_set_text(label, text)
    label.text = text or ""
end

function lv.label_set_long_mode(label, mode)
    label.long_mode = mode
end

function lv.bar_set_range(bar, min_value, max_value)
    bar.min_value, bar.max_value = min_value, max_value
end

function lv.bar_set_value(bar, value)
    bar.value = value
end

function lv.event_get_code(event)
    return event and event.code or 0
end

function lv.batch_begin()
end

function lv.batch_end()
end

function lv.poll_events()
    return 0
end

setmetatable(lv, {
    __index = function(_, key)
        if type(key) == "string" and key:match("^obj_set_style_") then
            return function(object, value)
                if object then
                    object.styles[key] = value
                end
            end
        end
        if type(key) == "string" and key:match("^label_set_") then
            return function(object, value)
                object[key] = value
            end
        end
        return nil
    end,
})

_G.lvgl = lv

-- sys -----------------------------------------------------------------
local sys = {
    _timers = {},
    _loops = {},
    _now = 0,
    _id = 0,
}

function sys.timerStart(callback, delay, ...)
    sys._id = sys._id + 1
    sys._timers[#sys._timers + 1] = {
        id = sys._id,
        at = sys._now + (delay or 0),
        callback = callback,
        args = { ... },
    }
    return sys._id
end

function sys.timerLoopStart(callback, delay, ...)
    sys._id = sys._id + 1
    sys._loops[#sys._loops + 1] = {
        id = sys._id,
        delay = delay,
        callback = callback,
        args = { ... },
    }
    return sys._id
end

function sys.timerStop(id)
    for index, timer in ipairs(sys._timers) do
        if timer.id == id then
            table.remove(sys._timers, index)
            return
        end
    end
end

function sys.run()
    local steps = 0
    while #sys._timers > 0 and steps < 200 do
        table.sort(sys._timers, function(a, b)
            return a.at < b.at
        end)
        local timer = table.remove(sys._timers, 1)
        sys._now = timer.at
        local ok, err = pcall(timer.callback, table.unpack(timer.args))
        if not ok then
            error("timer failed: " .. tostring(err), 0)
        end
        steps = steps + 1
    end
end

package.preload["sys"] = function()
    return sys
end

-- fskv ----------------------------------------------------------------
local fskv_store = {}
local fskv = {}

function fskv.init()
    return true
end

function fskv.get(key)
    return fskv_store[key]
end

function fskv.set(key, value)
    fskv_store[key] = value
    return true
end

_G.fskv = fskv

-- fs ------------------------------------------------------------------
local tree = {
    ["/sdcard"] = { "Books" },
    ["/sdcard/Books"] = { "测试小说.txt" },
}

local fs = {}

function fs.listdir(path)
    local entries = tree[path]
    if not entries then
        return nil, "no such directory"
    end
    local copy = {}
    for index, name in ipairs(entries) do
        copy[index] = name
    end
    return copy
end

function fs.isdir(path)
    return tree[path] ~= nil
end

_G.fs = fs

-- io: 把 /sdcard/Books/测试小说.txt 映射到真实临时文件 -----------------
local sample_path = (os.getenv("TEMP") or ".") .. "/lemonade_novel_sample.txt"
local lines = {}
for i = 1, 80 do
    lines[i] = string.format("这是第 %02d 行，用来测试小说阅读器的分页和进度保存。", i)
end
local sample_file = io.open(sample_path, "wb")
assert(sample_file, "cannot create temp sample")
sample_file:write(table.concat(lines, "\n"))
sample_file:close()

local real_open = io.open
io.open = function(path, mode)
    if path == "/sdcard/Books/测试小说.txt" then
        path = sample_path
    end
    return real_open(path, mode)
end

-- app / ui / log -------------------------------------------------------
_G.app = {
    data_path = function(relative)
        return "/sdcard/var/opt/com.lemonade.novel/data/" .. relative
    end,
}

local ui_state = {
    topbar_shown = 0,
    topbar_hidden = 0,
    notices = {},
}

_G.ui = {
    show_topbar = function()
        ui_state.topbar_shown = ui_state.topbar_shown + 1
    end,
    hide_topbar = function()
        ui_state.topbar_hidden = ui_state.topbar_hidden + 1
    end,
    show_notification = function(message, suffix, delay)
        ui_state.notices[#ui_state.notices + 1] = {
            message = message,
            suffix = suffix,
            delay = delay,
        }
    end,
}

_G.log = {
    debug = function() end,
    info = function() end,
    warn = function() end,
    error = function(_, message)
        error("log.error: " .. tostring(message), 0)
    end,
}

-- helpers -------------------------------------------------------------
local function walk(object, callback)
    callback(object)
    for _, child in ipairs(object.children) do
        walk(child, callback)
    end
end

local function find_button_with_label(text)
    local found = nil
    walk(page, function(object)
        if object.kind == "btn" then
            for _, child in ipairs(object.children) do
                if child.kind == "label" and child.text == text then
                    found = object
                end
            end
        end
    end)
    return found
end

local function find_label_containing(text)
    local found = nil
    walk(page, function(object)
        if object.kind == "label" and object.text:find(text, 1, true) then
            found = object
        end
    end)
    return found
end

local function fire(object, code)
    assert(object, "object not found")
    local callback = object.callbacks[code]
    assert(callback, "callback not found")
    callback({ code = code })
end

-- run -----------------------------------------------------------------
dofile("main.lua")

-- 初始扫描后应该能看到书名
local row = find_button_with_label("测试小说")
assert(row, "book row should exist")
assert(find_label_containing("Books"), "shelf meta should show folder")
assert(find_label_containing("未读"), "shelf should show unread state")

-- 打开书
fire(row, lv.EVENT_CLICKED)
sys.run()

local content = find_label_containing("这是第 01 行")
assert(content, "first page content should be rendered")
assert(ui_state.topbar_shown > 0, "reader should keep topbar visible")
assert(find_label_containing("1/"), "page count should be shown")

-- 下一页
local next_button = find_button_with_label("下一页")
assert(next_button, "next button should exist")
fire(next_button, lv.EVENT_CLICKED)
sys.run()

local function parse_progress(value)
    local offset, page, total, size = value:match("^(%d+):(%d+):(%d+):(%d+)$")
    assert(offset, "progress should be stored as offset:page:total:size")
    return {
        o = tonumber(offset),
        p = tonumber(page),
        t = tonumber(total),
        s = tonumber(size),
    }
end

local record = parse_progress(fskv_store["novel:/sdcard/Books/测试小说.txt"])
assert(record.o > 0, "progress should be saved after page turn")
assert(record.p == 2, "page index should advance")
assert(find_label_containing("2/"), "page count should advance")
local saved_offset = record.o

-- 返回书架
local back_button = find_button_with_label("返回")
assert(back_button, "back button should exist")
fire(back_button, lv.EVENT_CLICKED)
sys.run()

assert(ui_state.topbar_shown > 0, "shelf should show topbar")
assert(find_button_with_label("测试小说"), "shelf should list the book again")
assert(find_label_containing("2/"), "shelf should display page progress")

-- 再次打开应该恢复到上次位置
row = find_button_with_label("测试小说")
fire(row, lv.EVENT_CLICKED)
sys.run()
local restored = find_label_containing("这是第")
assert(restored, "restored page should have content")
assert(parse_progress(fskv_store["novel:/sdcard/Books/测试小说.txt"]).o == saved_offset,
    "offset should not change on open")

print("app smoke test passed")
