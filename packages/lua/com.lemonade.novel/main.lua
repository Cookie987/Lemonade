-- 小说阅读器：扫描 SD 卡上的 TXT/MD 文件，按屏分页，并把阅读位置保存到 fskv。
local sys = require("sys")
local lv = lvgl
local core = require("reader_core")

local page = lv.app_page()
if not page then
    return
end

local COLOR = {
    bg = 0xF7F1E5,
    card = 0xFFFCF6,
    card_alt = 0xFFF8EC,
    header = 0xEFE4D2,
    reader_bg = 0xF8F2E6,
    reader_text = 0x2B2620,
    text = 0x2B2620,
    muted = 0x7D7264,
    faint = 0xB7A995,
    accent = 0x9A4E2E,
    accent_soft = 0xC98A63,
    line = 0xE4D8C5,
    bar_bg = 0xE9DDCA,
    white = 0xFFFFFF,
}

local BOOKS_PER_PAGE_WITH_RESUME = 3
local BOOKS_PER_PAGE = 4
local SHELF_ROW_HEIGHT = 36
local SHELF_ROW_GAP = 4

local SCAN_ROOTS = {
    "/sdcard/Books",
    "/sdcard/books",
    "/sdcard/Novels",
    "/sdcard/novels",
    "/sdcard/小说",
    "/sdcard/电子书",
    "/sdcard/Download",
    "/sdcard/Documents",
    "/sdcard/Text",
    "/sdcard/txt",
}

local SKIP_DIRS = {
    ["opt"] = true,
    ["var"] = true,
    ["tmp"] = true,
    ["lib"] = true,
    ["music"] = true,
    ["config"] = true,
    ["cache"] = true,
    ["logs"] = true,
    ["log"] = true,
    ["assets"] = true,
    ["fonts"] = true,
    ["imgs"] = true,
    ["esphome"] = true,
    ["system volume information"] = true,
    ["$recycle.bin"] = true,
    [".trashes"] = true,
    [".spotlight-v100"] = true,
}

local state = {
    view = "shelf",
    books = {},
    book_index = {},
    shelf_page = 1,
    scanning = false,
    last_book = nil,
    persist = false,
    persist_error = false,
    notify_save_failed = false,

    file = nil,
    path = nil,
    size = 0,
    start_offset = 0,
    pos = 0,
    next_pos = 0,
    at_eof = false,
    history = {},
    dirty = false,
    save_timer = nil,
}

local refs = {}
local show_shelf

local function with_batch(fn)
    lv.batch_begin()
    local ok, result = pcall(fn)
    lv.batch_end()
    if not ok then
        error(result)
    end
    return result
end

local function notify(message, delay)
    ui.show_notification(message or "", "novel", delay or 1800)
end

local function schedule(fn, delay)
    return sys.timerStart(function()
        local ok, err = pcall(fn)
        if not ok then
            log.error("novel", tostring(err))
            notify("操作失败，请重试", 2000)
        end
    end, delay or 20)
end

local function create_label(parent, text, x, y, width, height, color, align)
    local label = lv.label_create(parent)
    lv.obj_set_pos(label, x, y)
    if width then
        lv.obj_set_width(label, width)
    end
    if height then
        lv.obj_set_height(label, height)
    end
    lv.label_set_text(label, text or "")
    lv.obj_set_style_text_color(label, color or COLOR.text, 0)
    if align then
        lv.obj_set_style_text_align(label, align, 0)
    end
    return label
end

local function create_button(parent, text, x, y, width, height, opts)
    opts = opts or {}
    local button = lv.btn_create(parent)
    lv.obj_set_pos(button, x, y)
    lv.obj_set_size(button, width, height)
    lv.obj_set_style_radius(button, opts.radius or 9, 0)
    lv.obj_set_style_border_width(button, 0, 0)
    lv.obj_set_style_shadow_width(button, 0, 0)
    lv.obj_set_style_outline_width(button, 0, 0)
    lv.obj_set_style_pad_all(button, 0, 0)
    if opts.transparent then
        lv.obj_set_style_bg_opa(button, lv.OPA_TRANSP, 0)
    else
        lv.obj_set_style_bg_opa(button, lv.OPA_COVER, 0)
        lv.obj_set_style_bg_color(button, opts.bg or COLOR.card, 0)
    end

    local label = lv.label_create(button)
    lv.label_set_text(label, text or "")
    lv.obj_set_style_text_color(label, opts.fg or COLOR.text, 0)
    if opts.dots then
        lv.obj_set_width(label, math.max(12, width - 12))
        lv.label_set_long_mode(label, lv.LABEL_LONG_MODE_DOTS)
    end
    lv.obj_center(label)

    if opts.on_click then
        lv.obj_add_event_cb(button, opts.on_click, lv.EVENT_CLICKED)
    end
    return button, label
end

local function create_bar(parent, x, y, width, height, min_value, max_value, color)
    local bar = lv.bar_create(parent)
    lv.obj_set_pos(bar, x, y)
    lv.obj_set_size(bar, width, height)
    lv.bar_set_range(bar, min_value or 0, max_value or 100)
    lv.bar_set_value(bar, min_value or 0, 0)
    lv.obj_set_style_bg_color(bar, COLOR.bar_bg, 0)
    lv.obj_set_style_bg_opa(bar, lv.OPA_COVER, 0)
    lv.obj_set_style_border_width(bar, 0, 0)
    lv.obj_set_style_radius(bar, math.floor(height / 2), 0)
    lv.obj_set_style_bg_color(bar, color or COLOR.accent, lv.PART_INDICATOR)
    lv.obj_set_style_bg_opa(bar, lv.OPA_COVER, lv.PART_INDICATOR)
    lv.obj_set_style_radius(bar, math.floor(height / 2), lv.PART_INDICATOR)
    return bar
end

local function record_progress(record)
    if type(record) ~= "table" then
        return "未读", 0
    end
    local offset = tonumber(record.o) or 0
    local size = tonumber(record.s) or 0
    if size <= 0 then
        return "未读", 0
    end
    local value = math.floor(offset / size * 100 + 0.5)
    if value < 0 then
        value = 0
    elseif value > 100 then
        value = 100
    end
    return core.percent(offset, size), value
end

local function get_record(path)
    if not state.persist or not path or not (fskv and fskv.get) then
        return nil
    end
    local ok, record = pcall(fskv.get, core.progress_key(path))
    if ok and type(record) == "table" then
        return record
    end
    return nil
end

local function persist_last_book(path)
    if not state.persist or not (fskv and fskv.set) then
        return
    end
    state.last_book = path
    local ok = pcall(fskv.set, "last_book", path)
    if not ok then
        log.warn("novel", "failed to save last_book")
    end
end

local function save_now()
    if not state.persist or not state.path or not (fskv and fskv.set) then
        return
    end
    if state.pos <= 0 and not state.dirty then
        return
    end

    local history = {}
    for i, value in ipairs(state.history) do
        history[i] = value
    end
    local record = {
        o = state.pos,
        s = state.size,
        h = history,
    }

    local ok, result = pcall(fskv.set, core.progress_key(state.path), record)
    if ok and result then
        state.dirty = false
        state.notify_save_failed = false
        return
    end

    log.warn("novel", "failed to save progress")
    state.dirty = true
    if not state.notify_save_failed then
        state.notify_save_failed = true
        notify("进度保存失败", 2200)
    end
    if not state.save_timer then
        state.save_timer = sys.timerStart(function()
            state.save_timer = nil
            save_now()
        end, 5000)
    end
end

local function mark_dirty()
    state.dirty = true
    if state.save_timer then
        sys.timerStop(state.save_timer)
        state.save_timer = nil
    end
    state.save_timer = sys.timerStart(function()
        state.save_timer = nil
        save_now()
    end, 1500)
end

local function push_history(position)
    state.history[#state.history + 1] = position
    while #state.history > core.HISTORY_LIMIT do
        table.remove(state.history, 1)
    end
end

local function pop_history()
    return table.remove(state.history)
end

local function read_at(file, position, length)
    if not file then
        return nil
    end
    local ok = file:seek("set", position)
    if not ok then
        return nil
    end
    return file:read(length)
end

local function snap_offset(file, position, size)
    position = core.clamp_offset(position, size)
    if position <= 0 or position >= size then
        return position
    end

    local start = math.max(0, position - 3)
    local chunk = read_at(file, start, 4) or ""
    local index = position - start + 1
    while index <= #chunk do
        local byte = string.byte(chunk, index)
        if not byte or byte < 0x80 or byte > 0xBF then
            return position
        end
        position = position + 1
        if position >= size then
            return size
        end
        index = index + 1
    end
    return position
end

local function snap_forward(file, position, limit)
    local start = position
    while start < limit do
        local chunk = read_at(file, start, 1)
        local byte = chunk and chunk:byte(1)
        if not byte or byte < 0x80 or byte > 0xBF then
            return start
        end
        start = start + 1
    end
    return start
end

local function find_prev_pos(file, position)
    if position <= 0 then
        return 0
    end

    local window = 16384
    local start = snap_forward(file, math.max(0, position - window), position)
    local chunk = read_at(file, start, position - start) or ""
    if chunk == "" then
        return 0
    end

    local relative = 0
    local last = nil
    while relative < #chunk do
        local _, used = core.paginate(chunk:sub(relative + 1))
        if not used or used <= 0 then
            break
        end
        local absolute = start + relative
        if absolute >= position then
            break
        end
        last = absolute
        relative = relative + used
    end
    return last or 0
end

local function read_page(position)
    if not state.file or position >= state.size then
        return "", state.size, true
    end

    local chunk = read_at(state.file, position, core.READ_CHUNK) or ""
    if chunk == "" then
        return "", state.size, true
    end

    local text, used, filled = core.paginate(chunk)
    while not filled and used >= #chunk and #chunk >= core.READ_CHUNK do
        local more = state.file:read(core.READ_CHUNK)
        if not more or more == "" then
            break
        end
        chunk = chunk .. more
        text, used, filled = core.paginate(chunk)
    end

    if not used or used <= 0 then
        return nil, position, true
    end

    local next_pos = position + used
    if next_pos > state.size then
        next_pos = state.size
    end
    return text, next_pos, next_pos >= state.size
end

local function load_page(position)
    local guard = 0
    while guard < 32 do
        local text, next_pos, at_eof = read_page(position)
        if text == nil then
            return nil, position, true
        end
        local has_content = text:find("%S") ~= nil
        if has_content or at_eof or next_pos <= position then
            return text, next_pos, at_eof
        end
        position = next_pos
        guard = guard + 1
    end
    return "", position, true
end

local function render_current()
    if not refs.content or not state.file then
        return
    end

    local text, next_pos, at_eof = load_page(state.pos)
    if text == nil then
        notify("读取文件失败", 2200)
        text = "（读取失败）"
        next_pos = state.pos
        at_eof = true
    end

    state.next_pos = next_pos
    state.at_eof = at_eof

    if text == "" and at_eof then
        text = "（全书完）"
    end

    local progress_pos = at_eof and state.size or state.pos
    local permille = 0
    if state.size > 0 then
        permille = math.floor(progress_pos / state.size * 1000 + 0.5)
        if permille < 0 then
            permille = 0
        elseif permille > 1000 then
            permille = 1000
        end
    end

    with_batch(function()
        lv.label_set_text(refs.content, text)
        lv.label_set_text(refs.percent, core.percent_precise(progress_pos, state.size))
        lv.bar_set_value(refs.progress, permille, 0)
    end)
end

local function go_next()
    if state.view ~= "reader" or not state.file then
        return
    end
    if state.at_eof then
        notify("已经读到结尾了", 1200)
        return
    end
    if not state.next_pos or state.next_pos <= state.pos then
        notify("没有下一页了", 1200)
        return
    end

    push_history(state.pos)
    state.pos = state.next_pos
    mark_dirty()
    render_current()
end

local function go_prev()
    if state.view ~= "reader" or not state.file then
        return
    end
    if state.pos <= state.start_offset then
        notify("已经是第一页了", 1200)
        return
    end

    local previous = pop_history()
    if not previous or previous >= state.pos then
        previous = find_prev_pos(state.file, state.pos)
    end
    if previous < state.start_offset then
        previous = state.start_offset
    end
    if previous >= state.pos then
        previous = state.start_offset
    end

    state.pos = previous
    mark_dirty()
    render_current()
end

local function close_reader()
    if state.save_timer then
        sys.timerStop(state.save_timer)
        state.save_timer = nil
    end
    if state.file then
        pcall(function()
            state.file:close()
        end)
    end
    state.file = nil
    state.path = nil
    state.size = 0
    state.start_offset = 0
    state.pos = 0
    state.next_pos = 0
    state.at_eof = false
    state.history = {}
    state.dirty = false
end

local function show_reader()
    refs = {}
    state.view = "reader"
    ui.hide_topbar()

    lv.obj_clean(page)
    lv.obj_set_style_bg_color(page, COLOR.reader_bg, 0)
    lv.obj_set_style_bg_opa(page, lv.OPA_COVER, 0)
    lv.obj_set_style_border_width(page, 0, 0)

    local header = lv.obj_create(page)
    lv.obj_set_pos(header, 0, 0)
    lv.obj_set_size(header, 320, 30)
    lv.obj_set_style_bg_color(header, COLOR.header, 0)
    lv.obj_set_style_bg_opa(header, lv.OPA_COVER, 0)
    lv.obj_set_style_border_width(header, 0, 0)
    lv.obj_set_style_radius(header, 0, 0)
    lv.obj_set_style_pad_all(header, 0, 0)

    create_button(header, "书架", 6, 3, 52, 24, {
        transparent = true,
        fg = COLOR.accent,
        radius = 7,
        on_click = function()
            schedule(function()
                save_now()
                close_reader()
                show_shelf()
            end)
        end,
    })

    refs.title = create_label(header, state.title or "阅读", 64, 7, 168, 16, COLOR.text, lv.TEXT_ALIGN_LEFT)
    lv.label_set_long_mode(refs.title, lv.LABEL_LONG_MODE_DOTS)
    refs.percent = create_label(header, "0.0%", 238, 7, 76, 16, COLOR.accent, lv.TEXT_ALIGN_RIGHT)

    local header_line = lv.obj_create(page)
    lv.obj_set_pos(header_line, 0, 30)
    lv.obj_set_size(header_line, 320, 1)
    lv.obj_set_style_bg_color(header_line, COLOR.line, 0)
    lv.obj_set_style_bg_opa(header_line, lv.OPA_COVER, 0)
    lv.obj_set_style_border_width(header_line, 0, 0)
    lv.obj_set_style_radius(header_line, 0, 0)

    refs.content = lv.label_create(page)
    lv.obj_set_pos(refs.content, 12, 36)
    lv.obj_set_size(refs.content, 296, 162)
    lv.label_set_long_mode(refs.content, lv.LABEL_LONG_MODE_CLIP)
    lv.obj_set_style_text_color(refs.content, COLOR.reader_text, 0)
    lv.obj_set_style_text_align(refs.content, lv.TEXT_ALIGN_LEFT, 0)
    lv.obj_set_style_text_line_space(refs.content, 3, 0)
    lv.obj_set_style_bg_opa(refs.content, lv.OPA_TRANSP, 0)

    local function add_tap_zone(x, width, callback)
        local zone = lv.obj_create(page)
        lv.obj_set_pos(zone, x, 34)
        lv.obj_set_size(zone, width, 166)
        lv.obj_set_style_bg_opa(zone, lv.OPA_TRANSP, 0)
        lv.obj_set_style_border_width(zone, 0, 0)
        lv.obj_set_style_radius(zone, 0, 0)
        lv.obj_add_flag(zone, lv.FLAG_CLICKABLE)
        lv.obj_add_event_cb(zone, function()
            callback()
        end, lv.EVENT_CLICKED)
        return zone
    end

    add_tap_zone(0, 74, go_prev)
    add_tap_zone(74, 246, go_next)

    local footer_line = lv.obj_create(page)
    lv.obj_set_pos(footer_line, 0, 202)
    lv.obj_set_size(footer_line, 320, 1)
    lv.obj_set_style_bg_color(footer_line, COLOR.line, 0)
    lv.obj_set_style_bg_opa(footer_line, lv.OPA_COVER, 0)
    lv.obj_set_style_border_width(footer_line, 0, 0)
    lv.obj_set_style_radius(footer_line, 0, 0)

    create_button(page, "上一页", 8, 206, 64, 28, {
        bg = COLOR.card,
        fg = COLOR.accent,
        radius = 8,
        on_click = go_prev,
    })
    create_button(page, "下一页", 248, 206, 64, 28, {
        bg = COLOR.accent,
        fg = COLOR.white,
        radius = 8,
        on_click = go_next,
    })

    create_label(page, "轻触左右翻页", 76, 209, 168, 18, COLOR.muted, lv.TEXT_ALIGN_CENTER)

    refs.progress = create_bar(page, 0, 236, 320, 4, 0, 1000, COLOR.accent)

    render_current()
end

local function open_book(path)
    if type(path) ~= "string" or path == "" then
        return
    end
    if state.view == "reader" and state.path == path then
        return
    end

    save_now()
    close_reader()

    local file, err = io.open(path, "rb")
    if not file then
        notify("无法打开文件：" .. tostring(err), 2400)
        show_shelf()
        return
    end

    local size = file:seek("end")
    if not size or size <= 0 then
        file:close()
        notify("这个文件是空的", 1800)
        show_shelf()
        return
    end

    file:seek("set", 0)
    local header = file:read(8192) or ""

    if header:sub(1, 2) == "\255\254" or header:sub(1, 2) == "\254\255" then
        file:close()
        notify("不支持 UTF-16，请转换成 UTF-8 编码", 2800)
        show_shelf()
        return
    end

    if not core.is_utf8(header, true) then
        file:close()
        notify("仅支持 UTF-8 编码的 TXT 文件", 2600)
        show_shelf()
        return
    end

    local _, bom_size = core.strip_bom(header)

    local record = get_record(path)
    local position = 0
    local history = {}
    if record then
        position = core.clamp_offset(record.o, size)
        if bom_size > 0 and position < bom_size then
            position = bom_size
        end
        if type(record.h) == "table" then
            for _, value in ipairs(record.h) do
                local offset = tonumber(value)
                if offset and offset >= 0 and offset < position then
                    history[#history + 1] = offset
                end
            end
        end
    end

    if position >= size then
        position = find_prev_pos(file, size)
    end
    position = snap_offset(file, position, size)

    state.title = core.title_from_path(path)
    state.file = file
    state.path = path
    state.size = size
    state.start_offset = bom_size
    state.pos = position
    state.next_pos = position
    state.at_eof = false
    state.history = history
    state.dirty = false

    persist_last_book(path)
    show_reader()

    if position > 0 then
        notify("已恢复上次阅读位置", 1200)
    end
end

local add_book

local function scan_sdcard_root(books, seen)
    local entries = fs.listdir("/sdcard")
    if type(entries) ~= "table" then
        return
    end
    for _, name in ipairs(entries) do
        if #books >= core.MAX_BOOKS then
            return
        end
        if type(name) == "string" and core.is_book_filename(name) and core.is_utf8(name, true) then
            add_book(books, seen, "/sdcard/" .. name)
        end
    end
end

add_book = function(books, seen, path)
    if #books >= core.MAX_BOOKS or seen[path] then
        return
    end
    local title = core.title_from_path(path)
    if title == "" or not core.is_utf8(title, true) then
        return
    end
    seen[path] = true
    books[#books + 1] = {
        path = path,
        title = title,
        folder = core.folder_from_path(path),
    }
end

local function scan_dir(path, depth, books, seen)
    if #books >= core.MAX_BOOKS then
        return
    end
    local entries, err = fs.listdir(path)
    if type(entries) ~= "table" then
        if err then
            log.debug("novel", "listdir failed: " .. tostring(err))
        end
        return
    end

    for _, name in ipairs(entries) do
        if #books >= core.MAX_BOOKS then
            return
        end
        if type(name) == "string" and name ~= "" and name:sub(1, 1) ~= "." then
            local child = path .. "/" .. name
            if fs.isdir(child) then
                if depth < 3 and not SKIP_DIRS[name:lower()] then
                    scan_dir(child, depth + 1, books, seen)
                end
            elseif core.is_book_filename(name) and core.is_utf8(name, true) then
                add_book(books, seen, child)
            end
        end
    end
end

local function scan_books()
    if not (fs and fs.listdir and fs.isdir) then
        return {}, "文件系统接口不可用"
    end

    local books = {}
    local seen = {}

    scan_sdcard_root(books, seen)

    for _, root in ipairs(SCAN_ROOTS) do
        if #books < core.MAX_BOOKS and fs.isdir(root) then
            scan_dir(root, 0, books, seen)
        end
    end

    if #books < core.MAX_BOOKS and app and app.data_path then
        local private_books = app.data_path("books")
        if private_books and fs.isdir(private_books) then
            scan_dir(private_books, 0, books, seen)
        end
    end

    table.sort(books, function(a, b)
        local left = a.title:lower()
        local right = b.title:lower()
        if left == right then
            return a.path < b.path
        end
        return left < right
    end)

    return books, nil
end

local function safe_scan_books()
    local ok, books, err = pcall(scan_books)
    if ok then
        return books, err
    end
    log.error("novel", "scan failed: " .. tostring(books))
    return {}, "扫描 SD 卡失败"
end

show_shelf = function()
    refs = {}
    state.view = "shelf"
    ui.show_topbar()

    lv.obj_clean(page)
    lv.obj_set_style_bg_color(page, COLOR.bg, 0)
    lv.obj_set_style_bg_opa(page, lv.OPA_COVER, 0)
    lv.obj_set_style_border_width(page, 0, 0)

    create_label(page, "我的书架", 12, 29, 180, 18, COLOR.accent, lv.TEXT_ALIGN_LEFT)
    create_button(page, "刷新", 252, 28, 56, 24, {
        bg = COLOR.card,
        fg = COLOR.accent,
        radius = 7,
        on_click = function()
            if not state.scanning then
                schedule(function()
                    state.scanning = true
                    state.shelf_page = 1
                    show_shelf()
                    sys.timerStart(function()
                        local books, err = safe_scan_books()
                        state.books = books
                        state.book_index = {}
                        for _, book in ipairs(books) do
                            state.book_index[book.path] = book
                        end
                        state.scanning = false
                        show_shelf()
                        if err then
                            notify(err, 2400)
                        end
                    end, 30)
                end)
            end
        end,
    })

    if state.scanning then
        local loading = create_label(page, "正在扫描 SD 卡…\n\n支持 Books / Novels / 小说 等目录", 10, 92, 300, 60,
            COLOR.muted, lv.TEXT_ALIGN_CENTER)
        lv.label_set_long_mode(loading, lv.LABEL_LONG_MODE_WRAP)
        lv.obj_set_style_text_line_space(loading, 4, 0)
        return
    end

    local continue_book = nil
    if state.last_book and state.book_index[state.last_book] then
        continue_book = state.book_index[state.last_book]
    end

    if #state.books == 0 then
        local empty = create_label(page,
            "没有找到小说\n\n把 UTF-8 编码的 .txt 文件放到\nSD 卡的 Books 或 Novels 目录\n然后点右上角刷新",
            10, 72, 300, 120, COLOR.muted, lv.TEXT_ALIGN_CENTER)
        lv.label_set_long_mode(empty, lv.LABEL_LONG_MODE_WRAP)
        lv.obj_set_style_text_line_space(empty, 4, 0)
        return
    end

    local per_page = continue_book and BOOKS_PER_PAGE_WITH_RESUME or BOOKS_PER_PAGE
    local total_pages = math.max(1, math.ceil(#state.books / per_page))
    if state.shelf_page < 1 then
        state.shelf_page = 1
    elseif state.shelf_page > total_pages then
        state.shelf_page = total_pages
    end

    local rows_start = 56
    if continue_book then
        local record = get_record(continue_book.path)
        local percent_text, percent_value = record_progress(record)
        local path = continue_book.path

        local card = lv.btn_create(page)
        lv.obj_set_pos(card, 8, 52)
        lv.obj_set_size(card, 304, 42)
        lv.obj_set_style_bg_color(card, COLOR.card, 0)
        lv.obj_set_style_bg_opa(card, lv.OPA_COVER, 0)
        lv.obj_set_style_border_width(card, 0, 0)
        lv.obj_set_style_shadow_width(card, 0, 0)
        lv.obj_set_style_outline_width(card, 0, 0)
        lv.obj_set_style_radius(card, 11, 0)
        lv.obj_set_style_pad_all(card, 0, 0)
        lv.obj_add_event_cb(card, function()
            schedule(function()
                open_book(path)
            end)
        end, lv.EVENT_CLICKED)

        local accent = lv.obj_create(card)
        lv.obj_set_pos(accent, 0, 6)
        lv.obj_set_size(accent, 4, 30)
        lv.obj_set_style_bg_color(accent, COLOR.accent, 0)
        lv.obj_set_style_bg_opa(accent, lv.OPA_COVER, 0)
        lv.obj_set_style_border_width(accent, 0, 0)
        lv.obj_set_style_radius(accent, 2, 0)

        create_label(card, "继续阅读", 12, 4, 120, 14, COLOR.accent, lv.TEXT_ALIGN_LEFT)
        local title = create_label(card, continue_book.title, 12, 18, 228, 16, COLOR.text, lv.TEXT_ALIGN_LEFT)
        lv.label_set_long_mode(title, lv.LABEL_LONG_MODE_DOTS)
        create_label(card, percent_text, 244, 4, 52, 14, COLOR.accent, lv.TEXT_ALIGN_RIGHT)
        local bar = create_bar(card, 12, 36, 280, 3, 0, 100, COLOR.accent)
        lv.bar_set_value(bar, percent_value, 0)

        rows_start = 100
    end

    local start_index = (state.shelf_page - 1) * per_page + 1
    for slot = 0, per_page - 1 do
        local book = state.books[start_index + slot]
        if book then
            local x = 8
            local y = rows_start + slot * (SHELF_ROW_HEIGHT + SHELF_ROW_GAP)
            local record = get_record(book.path)
            local percent_text, percent_value = record_progress(record)
            local path = book.path

            local row = lv.btn_create(page)
            lv.obj_set_pos(row, x, y)
            lv.obj_set_size(row, 304, SHELF_ROW_HEIGHT)
            lv.obj_set_style_bg_color(row, COLOR.card, 0)
            lv.obj_set_style_bg_opa(row, lv.OPA_COVER, 0)
            lv.obj_set_style_border_width(row, 0, 0)
            lv.obj_set_style_shadow_width(row, 0, 0)
            lv.obj_set_style_outline_width(row, 0, 0)
            lv.obj_set_style_radius(row, 9, 0)
            lv.obj_set_style_pad_all(row, 0, 0)
            lv.obj_add_event_cb(row, function()
                schedule(function()
                    open_book(path)
                end)
            end, lv.EVENT_CLICKED)

            local title = create_label(row, book.title, 10, 3, 224, 16, COLOR.text, lv.TEXT_ALIGN_LEFT)
            lv.label_set_long_mode(title, lv.LABEL_LONG_MODE_DOTS)
            local meta = create_label(row, book.folder .. " · " .. percent_text, 10, 18, 224, 13,
                COLOR.muted, lv.TEXT_ALIGN_LEFT)
            lv.label_set_long_mode(meta, lv.LABEL_LONG_MODE_DOTS)
            local bar = create_bar(row, 10, 32, 218, 3, 0, 100, COLOR.accent)
            lv.bar_set_value(bar, percent_value, 0)
            create_label(row, ">", 282, 8, 14, 18, COLOR.faint, lv.TEXT_ALIGN_RIGHT)
        end
    end

    if total_pages > 1 then
        create_button(page, "上一页", 8, 218, 52, 20, {
            transparent = true,
            fg = state.shelf_page > 1 and COLOR.accent or COLOR.faint,
            radius = 6,
            on_click = function()
                if state.shelf_page > 1 then
                    schedule(function()
                        state.shelf_page = state.shelf_page - 1
                        show_shelf()
                    end)
                end
            end,
        })
        create_button(page, "下一页", 260, 218, 52, 20, {
            transparent = true,
            fg = state.shelf_page < total_pages and COLOR.accent or COLOR.faint,
            radius = 6,
            on_click = function()
                if state.shelf_page < total_pages then
                    schedule(function()
                        state.shelf_page = state.shelf_page + 1
                        show_shelf()
                    end)
                end
            end,
        })
    end

    local info = string.format("%d/%d · 共 %d 本", state.shelf_page, total_pages, #state.books)
    create_label(page, info, 64, 220, 192, 16, COLOR.muted, lv.TEXT_ALIGN_CENTER)
end

local function refresh_books()
    state.scanning = true
    state.shelf_page = 1
    show_shelf()
    sys.timerStart(function()
        local books, err = safe_scan_books()
        state.books = books
        state.book_index = {}
        for _, book in ipairs(books) do
            state.book_index[book.path] = book
        end
        state.scanning = false
        show_shelf()
        if err then
            notify(err, 2400)
        end
    end, 30)
end

-- 启动 ---------------------------------------------------------------
if fskv and fskv.init then
    local ok, result = pcall(fskv.init)
    state.persist = ok and result == true
    if state.persist then
        local read_ok, last = pcall(fskv.get, "last_book")
        if read_ok and type(last) == "string" and last ~= "" then
            state.last_book = last
        end
    else
        state.persist_error = true
    end
else
    state.persist_error = true
end

lv.obj_add_event_cb(page, function(e)
    if lv.event_get_code(e) == lv.EVENT_SCREEN_UNLOAD_START then
        save_now()
        close_reader()
        ui.show_topbar()
    end
end, lv.EVENT_SCREEN_UNLOAD_START)

show_shelf()
sys.timerStart(refresh_books, 30)
if state.persist_error then
    sys.timerStart(function()
        notify("进度存储不可用，阅读位置不会保存", 2600)
    end, 800)
end

sys.timerLoopStart(function()
    lv.poll_events(10)
end, 16)

sys.run()
