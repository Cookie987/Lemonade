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

local BOOKS_PER_PAGE = 3
local SHELF_ROW_HEIGHT = 48
local SHELF_ROW_GAP = 4

-- FAT 文件名不区分大小写，这里每个目录只保留一种写法，避免重复扫描。
local SCAN_ROOTS = {
    "/sdcard/Books",
    "/sdcard/Novels",
    "/sdcard/小说",
    "/sdcard/电子书",
    "/sdcard/Download",
    "/sdcard/Documents",
    "/sdcard/Text",
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
    shelf_page = 1,
    scanning = false,
    persist = false,
    persist_error = false,
    notify_save_failed = false,

    file = nil,
    path = nil,
    title = nil,
    size = 0,
    pos = 0,
    pages = {},
    page_index = 1,
    total_pages = 0,
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

    local page = tonumber(record.p)
    local total = tonumber(record.t)
    if page and total and total > 0 then
        return string.format("%d/%d", page, total), value
    end
    return core.percent(offset, size), value
end

local function progress_file_path(book_path)
    if not (app and app.data_path) then
        return nil
    end
    if app.mkdir then
        pcall(app.mkdir, "progress")
    end
    return app.data_path("progress/" .. core.hash_text(book_path) .. ".txt")
end

local function parse_record(text)
    if type(text) ~= "string" then
        return nil
    end
    local offset, page, total, size = text:match("^(%d+):(%d+):(%d+):(%d+)$")
    if not offset then
        return nil
    end
    return {
        o = tonumber(offset),
        p = tonumber(page),
        t = tonumber(total),
        s = tonumber(size),
    }
end

local function read_progress_file(book_path)
    local path = progress_file_path(book_path)
    if not path then
        return nil
    end
    local file = io.open(path, "rb")
    if not file then
        return nil
    end
    local data = file:read(128)
    file:close()
    return parse_record(data)
end

local function write_progress_file(book_path, text)
    local path = progress_file_path(book_path)
    if not path then
        return false
    end
    local file = io.open(path, "wb")
    if not file then
        return false
    end
    local ok, written = pcall(function()
        return file:write(text)
    end)
    file:close()
    return ok and written ~= nil
end

local function get_record(path)
    if not path then
        return nil
    end

    local record = read_progress_file(path)
    if record then
        return record
    end

    if not state.persist or not (fskv and fskv.get) then
        return nil
    end
    local ok, value = pcall(fskv.get, core.progress_key(path))
    if not ok then
        return nil
    end
    if type(value) == "table" then
        return value
    end
    return parse_record(value)
end

local function save_now()
    if not state.path then
        return
    end

    -- 优先写应用私有文件；fskv 作为兼容回退。
    local record = string.format("%d:%d:%d:%d",
        state.pos, state.page_index, state.total_pages, state.size)
    if write_progress_file(state.path, record) then
        state.dirty = false
        state.notify_save_failed = false
        return
    end

    if state.persist and fskv and fskv.set then
        local ok, result = pcall(fskv.set, core.progress_key(state.path), record)
        if ok and result then
            state.dirty = false
            state.notify_save_failed = false
            return
        end
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

local CACHE_MAGIC = 0x4E564C31

local function cache_file_path(book_path)
    if not (app and app.data_path) then
        return nil
    end
    if app.mkdir then
        pcall(app.mkdir, "cache")
    end
    return app.data_path("cache/" .. core.hash_text(book_path) .. ".pag")
end

local function load_page_cache(book_path, size, checksum)
    local cache_path = cache_file_path(book_path)
    if not cache_path then
        return nil
    end

    local file = io.open(cache_path, "rb")
    if not file then
        return nil
    end

    local header = file:read(16)
    if not header or #header < 16 then
        file:close()
        return nil
    end

    local ok, magic, cached_size, cached_checksum, count =
        pcall(string.unpack, "<I4I4I4I4", header)
    if not ok or magic ~= CACHE_MAGIC or cached_size ~= size or cached_checksum ~= checksum then
        file:close()
        return nil
    end
    if not count or count < 1 or count > 500000 or count > size then
        file:close()
        return nil
    end

    local data = file:read(count * 4)
    file:close()
    if not data or #data < count * 4 then
        return nil
    end

    local pages = {}
    local position = 1
    for index = 1, count do
        local value
        value, position = string.unpack("<I4", data, position)
        pages[index] = value
        if index > 1 and value <= pages[index - 1] then
            return nil
        end
    end
    if pages[1] > size or pages[count] > size then
        return nil
    end
    return pages
end

local function save_page_cache(book_path, size, checksum, pages)
    local cache_path = cache_file_path(book_path)
    if not cache_path then
        return
    end

    local file = io.open(cache_path, "wb")
    if not file then
        return
    end

    local ok = pcall(function()
        local chunks = {
            string.pack("<I4I4I4I4", CACHE_MAGIC, size, checksum, #pages),
        }
        for index = 1, #pages do
            chunks[index + 1] = string.pack("<I4", pages[index])
        end
        file:write(table.concat(chunks))
    end)
    file:close()
    if not ok then
        log.warn("novel", "failed to write page cache")
    end
end

-- 顺序扫一遍全书，记录每一页的起始字节位置。这样页码和总页数都是准确的。
local function build_pages(file, start_offset, size)
    local pages = {}
    local buffer = ""
    local base = start_offset
    local eof = false

    local function append_more()
        if eof then
            return
        end
        local more = read_at(file, base + #buffer, core.READ_CHUNK)
        if more and #more > 0 then
            buffer = buffer .. more
            if #more < core.READ_CHUNK then
                eof = true
            end
        else
            eof = true
        end
    end

    while base < size do
        pages[#pages + 1] = base

        while #buffer < core.READ_CHUNK and not eof do
            append_more()
        end
        if buffer == "" then
            break
        end

        local _, used, filled = core.paginate(buffer, nil, nil, false)
        while not filled and not eof do
            append_more()
            _, used, filled = core.paginate(buffer, nil, nil, false)
        end
        if not used or used <= 0 then
            break
        end

        base = base + used
        buffer = buffer:sub(used + 1)
    end

    if #pages == 0 then
        pages[1] = start_offset
    end
    return pages
end

local function find_page_index(pages, offset)
    local low, high = 1, #pages
    while low < high do
        local middle = math.floor((low + high + 1) / 2)
        if pages[middle] <= offset then
            low = middle
        else
            high = middle - 1
        end
    end
    return low
end

local function read_page_at(position)
    if not state.file or position >= state.size then
        return ""
    end

    local chunk = read_at(state.file, position, core.READ_CHUNK) or ""
    if chunk == "" then
        return ""
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
    return text or ""
end

local function render_current()
    if not refs.content or not state.file then
        return
    end

    local index = state.page_index
    local total = state.total_pages
    local position = state.pages[index] or 0
    local text = read_page_at(position)
    state.pos = position

    local permille = 0
    if total > 0 then
        permille = math.floor(index / total * 1000 + 0.5)
        if permille < 0 then
            permille = 0
        elseif permille > 1000 then
            permille = 1000
        end
    end

    with_batch(function()
        lv.label_set_text(refs.content, text)
        lv.label_set_text(refs.filename, state.title or "")
        lv.label_set_text(refs.pageinfo, string.format("%d/%d", index, total))
        lv.bar_set_value(refs.progress, permille, 0)
    end)
end

local function go_next()
    if state.view ~= "reader" or not state.file then
        return
    end
    if state.page_index >= state.total_pages then
        notify("已经是最后一页了", 1200)
        return
    end
    state.page_index = state.page_index + 1
    mark_dirty()
    render_current()
    save_now()
end

local function go_prev()
    if state.view ~= "reader" or not state.file then
        return
    end
    if state.page_index <= 1 then
        notify("已经是第一页了", 1200)
        return
    end
    state.page_index = state.page_index - 1
    mark_dirty()
    render_current()
    save_now()
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
    state.title = nil
    state.size = 0
    state.pos = 0
    state.pages = {}
    state.page_index = 1
    state.total_pages = 0
    state.dirty = false
end

local function show_reader()
    refs = {}
    state.view = "reader"
    ui.show_topbar()

    lv.obj_clean(page)
    lv.obj_set_style_bg_color(page, COLOR.reader_bg, 0)
    lv.obj_set_style_bg_opa(page, lv.OPA_COVER, 0)
    lv.obj_set_style_border_width(page, 0, 0)

    create_button(page, "返回", 6, 26, 52, 22, {
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

    refs.content = lv.label_create(page)
    lv.obj_set_pos(refs.content, 12, 50)
    lv.obj_set_size(refs.content, 296, 148)
    lv.label_set_long_mode(refs.content, lv.LABEL_LONG_MODE_CLIP)
    lv.obj_set_style_text_color(refs.content, COLOR.reader_text, 0)
    lv.obj_set_style_text_align(refs.content, lv.TEXT_ALIGN_LEFT, 0)
    lv.obj_set_style_text_line_space(refs.content, 2, 0)
    lv.obj_set_style_bg_opa(refs.content, lv.OPA_TRANSP, 0)

    local function add_tap_zone(x, width, callback)
        local zone = lv.btn_create(page)
        lv.obj_set_pos(zone, x, 50)
        lv.obj_set_size(zone, width, 148)
        lv.obj_set_style_bg_opa(zone, lv.OPA_TRANSP, 0)
        lv.obj_set_style_border_width(zone, 0, 0)
        lv.obj_set_style_shadow_width(zone, 0, 0)
        lv.obj_set_style_outline_width(zone, 0, 0)
        lv.obj_set_style_radius(zone, 0, 0)
        lv.obj_set_style_pad_all(zone, 0, 0)
        lv.obj_add_flag(zone, lv.FLAG_CLICKABLE)
        lv.obj_add_event_cb(zone, function()
            callback()
        end, lv.EVENT_CLICKED)
        return zone
    end

    add_tap_zone(0, 72, go_prev)
    add_tap_zone(72, 248, go_next)

    local footer_line = lv.obj_create(page)
    lv.obj_set_pos(footer_line, 0, 200)
    lv.obj_set_size(footer_line, 320, 1)
    lv.obj_set_style_bg_color(footer_line, COLOR.line, 0)
    lv.obj_set_style_bg_opa(footer_line, lv.OPA_COVER, 0)
    lv.obj_set_style_border_width(footer_line, 0, 0)
    lv.obj_set_style_radius(footer_line, 0, 0)

    create_button(page, "上一页", 4, 204, 56, 30, {
        bg = COLOR.card,
        fg = COLOR.accent,
        radius = 8,
        on_click = go_prev,
    })
    create_button(page, "下一页", 260, 204, 56, 30, {
        bg = COLOR.accent,
        fg = COLOR.white,
        radius = 8,
        on_click = go_next,
    })

    refs.filename = create_label(page, state.title or "", 64, 203, 192, 16, COLOR.text, lv.TEXT_ALIGN_CENTER)
    lv.label_set_long_mode(refs.filename, lv.LABEL_LONG_MODE_DOTS)
    refs.pageinfo = create_label(page, "1/1", 64, 221, 192, 16, COLOR.muted, lv.TEXT_ALIGN_CENTER)

    refs.progress = create_bar(page, 0, 238, 320, 2, 0, 1000, COLOR.accent)

    render_current()
end

local function show_loading()
    refs = {}
    state.view = "reader"
    ui.show_topbar()

    lv.obj_clean(page)
    lv.obj_set_style_bg_color(page, COLOR.reader_bg, 0)
    lv.obj_set_style_bg_opa(page, lv.OPA_COVER, 0)
    lv.obj_set_style_border_width(page, 0, 0)

    create_label(page, "正在排版…", 10, 106, 300, 24, COLOR.muted, lv.TEXT_ALIGN_CENTER)
    lv.poll_events(10)
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
    if size <= bom_size then
        file:close()
        notify("这个文件是空的", 1800)
        show_shelf()
        return
    end

    local tail = ""
    if size > 512 then
        file:seek("set", size - 512)
        tail = file:read(512) or ""
    end
    local checksum = core.hash32(
        "novel-v1-" .. core.PAGE_LINES .. "-" .. core.LINE_UNITS .. header .. tostring(size) .. tail
    )

    local pages = load_page_cache(path, size, checksum)
    if not pages then
        show_loading()
        local ok, built = pcall(build_pages, file, bom_size, size)
        if not ok or type(built) ~= "table" or #built == 0 then
            file:close()
            notify("排版失败，请检查文件", 2200)
            show_shelf()
            return
        end
        pages = built
        save_page_cache(path, size, checksum, pages)
    end

    local record = get_record(path)
    local position = record and core.clamp_offset(record.o, size) or 0
    if position < bom_size then
        position = bom_size
    end
    local page_index = find_page_index(pages, position)

    state.title = core.title_from_path(path)
    state.file = file
    state.path = path
    state.size = size
    state.pos = pages[page_index] or bom_size
    state.pages = pages
    state.page_index = page_index
    state.total_pages = #pages
    state.dirty = false

    show_reader()
    save_now()
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
    local key = path:lower()
    if #books >= core.MAX_BOOKS or seen[key] then
        return
    end
    local title = core.title_from_path(path)
    if title == "" or not core.is_utf8(title, true) then
        return
    end
    seen[key] = true
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

    if #state.books == 0 then
        local empty = create_label(page,
            "没有找到小说\n\n把 UTF-8 编码的 .txt 文件放到\nSD 卡的 Books 或 Novels 目录\n然后点右上角刷新",
            10, 72, 300, 120, COLOR.muted, lv.TEXT_ALIGN_CENTER)
        lv.label_set_long_mode(empty, lv.LABEL_LONG_MODE_WRAP)
        lv.obj_set_style_text_line_space(empty, 4, 0)
        return
    end

    local per_page = BOOKS_PER_PAGE
    local total_pages = math.max(1, math.ceil(#state.books / per_page))
    if state.shelf_page < 1 then
        state.shelf_page = 1
    elseif state.shelf_page > total_pages then
        state.shelf_page = total_pages
    end

    local rows_start = 54
    local start_index = (state.shelf_page - 1) * per_page + 1
    for slot = 0, per_page - 1 do
        local book = state.books[start_index + slot]
        if book then
            local y = rows_start + slot * (SHELF_ROW_HEIGHT + SHELF_ROW_GAP)
            local record = get_record(book.path)
            local percent_text, percent_value = record_progress(record)
            local path = book.path

            local row = lv.btn_create(page)
            lv.obj_set_pos(row, 12, y)
            lv.obj_set_size(row, 296, SHELF_ROW_HEIGHT)
            lv.obj_set_style_bg_color(row, COLOR.card, 0)
            lv.obj_set_style_bg_opa(row, lv.OPA_COVER, 0)
            lv.obj_set_style_border_width(row, 0, 0)
            lv.obj_set_style_shadow_width(row, 0, 0)
            lv.obj_set_style_outline_width(row, 0, 0)
            lv.obj_set_style_radius(row, 11, 0)
            lv.obj_set_style_pad_all(row, 0, 0)
            lv.obj_add_event_cb(row, function()
                schedule(function()
                    open_book(path)
                end)
            end, lv.EVENT_CLICKED)

            local title = create_label(row, book.title, 12, 5, 190, 18, COLOR.text, lv.TEXT_ALIGN_LEFT)
            lv.label_set_long_mode(title, lv.LABEL_LONG_MODE_DOTS)
            local meta = create_label(row, book.folder, 12, 25, 218, 14, COLOR.muted, lv.TEXT_ALIGN_LEFT)
            lv.label_set_long_mode(meta, lv.LABEL_LONG_MODE_DOTS)
            create_label(row, percent_text, 236, 5, 48, 18, COLOR.accent, lv.TEXT_ALIGN_RIGHT)
            local bar = create_bar(row, 12, 42, 272, 3, 0, 100, COLOR.accent)
            lv.bar_set_value(bar, percent_value, 0)
        end
    end

    if total_pages > 1 then
        create_button(page, "上一页", 8, 212, 52, 22, {
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
        create_button(page, "下一页", 260, 212, 52, 22, {
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
    create_label(page, info, 64, 214, 192, 18, COLOR.muted, lv.TEXT_ALIGN_CENTER)
end

local function refresh_books()
    state.scanning = true
    state.shelf_page = 1
    show_shelf()
    sys.timerStart(function()
        local books, err = safe_scan_books()
        state.books = books
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
end
local file_store = app and app.data_path ~= nil
state.persist_error = not (state.persist or file_store)

-- 回主页时只保存进度，不关闭文件，这样再次点开应用还能继续翻页。
lv.obj_add_event_cb(page, function(e)
    if lv.event_get_code(e) == lv.EVENT_SCREEN_UNLOAD_START then
        save_now()
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
