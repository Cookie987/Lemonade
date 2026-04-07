-- com.lemonade.2048

local sys = require("sys")
local lv = lvgl

local page = lv.app_page()
if not page then return end

lv.obj_clean(page)

local MATRIX_SIZE = 4
local BOARD_SIZE = 204
local BOARD_PAD = 8
local TILE_GAP = 5
local TILE_SIZE = 43
local BOARD_X_OFFSET = 35
local ANIM_FRAME_MS = 16
local SPAWN_ANIM_MS = 140
local TILE_ZOOM_DEFAULT = 256
local TILE_ZOOM_SPAWN_START = 96
local TILE_OPA_DEFAULT = 255
local TILE_OPA_SPAWN_START = 96

local COLORS = {
    bg = 0xFAF8EF,
    panel = 0xBBADA0,
    panel_shadow = 0xB39A88,
    text_dark = 0x6C635B,
    text_light = 0xF8F5F0,
    tile_empty = 0xCDC1B4,
    tile = {
        [0] = 0xCDC1B4,
        [1] = 0xEEE4DA,
        [2] = 0xEDE0C8,
        [3] = 0xF2B179,
        [4] = 0xF59563,
        [5] = 0xF67C5F,
        [6] = 0xF75F3B,
        [7] = 0xEDCF72,
        [8] = 0xEDCC61,
        [9] = 0xEDC850,
        [10] = 0xEDC53F,
        [11] = 0xEDC22E,
    },
    action = 0x8F7A66,
    restart = 0x8D6E63,
}

local state = {
    score = 0,
    game_over = false,
    board = {},
    tiles = {},
    tile_labels = {},
    status_label = nil,
    score_label = nil,
    best_label = nil,
    overlay = nil,
    overlay_label = nil,
    touch_start = nil,
    anim_timer = nil,
    animations = {},
    animating = false,
}

local update_board_ui

local function with_batch(fn)
    lv.batch_begin()
    local ok, result = pcall(fn)
    lv.batch_end()
    if not ok then
        error(result)
    end
    return result
end

local function dbg(...)
    -- local parts = {}
    -- for i = 1, select("#", ...) do
    --     parts[#parts + 1] = tostring(select(i, ...))
    -- end
    -- local message = "[2048] " .. table.concat(parts, " ")

    -- if type(log) == "table" and log.info then
    --     log.info(message)
    -- elseif type(log) == "function" then
    --     log(message)
    -- end
end

local function pow2(exp)
    if exp <= 0 then
        return 0
    end
    local value = 1
    for _ = 1, exp do
        value = value * 2
    end
    return value
end

local function get_tile_color(exp)
    return COLORS.tile[exp] or 0x3C3A32
end

local function get_tile_text_color(exp)
    if exp <= 2 then
        return COLORS.text_dark
    end
    return COLORS.text_light
end

local function board_value_text(exp)
    if exp == 0 then
        return ""
    end
    return tostring(pow2(exp))
end

local function clamp(value, min_value, max_value)
    if value < min_value then
        return min_value
    end
    if value > max_value then
        return max_value
    end
    return value
end

local function round(value)
    if value >= 0 then
        return math.floor(value + 0.5)
    end
    return math.ceil(value - 0.5)
end

local function lerp(from_value, to_value, progress)
    return from_value + (to_value - from_value) * progress
end

local function ease_out_cubic(t)
    local inv = 1 - t
    return 1 - inv * inv * inv
end

local function ease_out_back(t)
    local c1 = 1.70158
    local c3 = c1 + 1
    local x = t - 1
    return 1 + c3 * x * x * x + c1 * x * x
end

local function set_tile_transform(tile, translate_x, translate_y, zoom, opa)
    lv.obj_set_style_translate_x(tile, round(translate_x or 0), 0)
    lv.obj_set_style_translate_y(tile, round(translate_y or 0), 0)
    lv.obj_set_style_transform_zoom(tile, round(zoom or TILE_ZOOM_DEFAULT), 0)
    lv.obj_set_style_opa(tile, round(opa or TILE_OPA_DEFAULT), 0)
end

local function set_tile_empty_cover(tile, hidden)
    if hidden then
        lv.obj_set_style_bg_opa(tile, 0, 0)
    else
        lv.obj_set_style_bg_opa(tile, TILE_OPA_DEFAULT, 0)
    end
end

local function reset_tile_transform(tile)
    set_tile_transform(tile, 0, 0, TILE_ZOOM_DEFAULT, TILE_OPA_DEFAULT)
    set_tile_empty_cover(tile, false)
end

local function reset_all_tile_transforms()
    with_batch(function()
        for row = 1, MATRIX_SIZE do
            for col = 1, MATRIX_SIZE do
                reset_tile_transform(state.tiles[row][col])
            end
        end
    end)
end

local function stop_animation_timer()
    if state.anim_timer then
        lv.timer_del(state.anim_timer)
        state.anim_timer = nil
    end
end

local function clear_animations()
    stop_animation_timer()
    state.animations = {}
    state.animating = false
    reset_all_tile_transforms()
end

local function count_empty(board)
    local count = 0
    for row = 1, MATRIX_SIZE do
        for col = 1, MATRIX_SIZE do
            if board[row][col] == 0 then
                count = count + 1
            end
        end
    end
    return count
end

local function find_target(array, x, stop)
    if x == 1 then
        return x
    end

    for t = x - 1, 1, -1 do
        if array[t] ~= 0 then
            if array[t] ~= array[x] then
                return t + 1
            end
            return t
        elseif t == stop then
            return t
        end
    end

    return x
end

local function slide_array(array)
    local success = false
    local stop = 1

    for x = 1, MATRIX_SIZE do
        if array[x] ~= 0 then
            local target = find_target(array, x, stop)
            if target ~= x then
                if array[target] == 0 then
                    array[target] = array[x]
                elseif array[target] == array[x] then
                    array[target] = array[target] + 1
                    state.score = state.score + pow2(array[target])
                    stop = target + 1
                end
                array[x] = 0
                success = true
            end
        end
    end

    return success
end

local function rotate_matrix(board)
    local n = MATRIX_SIZE
    for i = 1, math.floor(n / 2) do
        for j = i, n - i do
            local temp = board[i][j]
            board[i][j] = board[j][n - i + 1]
            board[j][n - i + 1] = board[n - i + 1][n - j + 1]
            board[n - i + 1][n - j + 1] = board[n - j + 1][i]
            board[n - j + 1][i] = temp
        end
    end
end

local function move_up(board)
    local success = false
    for row = 1, MATRIX_SIZE do
        if slide_array(board[row]) then
            success = true
        end
    end
    return success
end

local function move_left(board)
    rotate_matrix(board)
    local success = move_up(board)
    rotate_matrix(board)
    rotate_matrix(board)
    rotate_matrix(board)
    return success
end

local function move_down(board)
    rotate_matrix(board)
    rotate_matrix(board)
    local success = move_up(board)
    rotate_matrix(board)
    rotate_matrix(board)
    return success
end

local function move_right(board)
    rotate_matrix(board)
    rotate_matrix(board)
    rotate_matrix(board)
    local success = move_up(board)
    rotate_matrix(board)
    return success
end

local function find_pair_down(board)
    for row = 1, MATRIX_SIZE do
        for col = 1, MATRIX_SIZE - 1 do
            if board[row][col] == board[row][col + 1] then
                return true
            end
        end
    end
    return false
end

local function is_game_over(board)
    if count_empty(board) > 0 then
        return false
    end

    if find_pair_down(board) then
        return false
    end

    rotate_matrix(board)
    local has_pair = find_pair_down(board)
    rotate_matrix(board)
    rotate_matrix(board)
    rotate_matrix(board)

    return not has_pair
end

local function get_best_tile(board)
    local best = 0
    for row = 1, MATRIX_SIZE do
        for col = 1, MATRIX_SIZE do
            if board[row][col] > best then
                best = board[row][col]
            end
        end
    end
    return pow2(best)
end

local function add_random(board)
    local empty = {}
    for row = 1, MATRIX_SIZE do
        for col = 1, MATRIX_SIZE do
            if board[row][col] == 0 then
                empty[#empty + 1] = { row = row, col = col }
            end
        end
    end

    if #empty == 0 then
        return false
    end

    local choice = empty[esp.random(#empty)]
    local exp = (esp.random(10) == 10) and 2 or 1
    board[choice.row][choice.col] = exp
    return {
        row = choice.row,
        col = choice.col,
        exp = exp,
    }
end

local function add_spawn_animation(animations, spawned_tile)
    if not spawned_tile then
        return
    end

    animations[#animations + 1] = {
        kind = "spawn",
        row = spawned_tile.row,
        col = spawned_tile.col,
        from_zoom = TILE_ZOOM_SPAWN_START,
        to_zoom = TILE_ZOOM_DEFAULT,
        from_opa = TILE_OPA_SPAWN_START,
        to_opa = TILE_OPA_DEFAULT,
        duration = SPAWN_ANIM_MS,
        elapsed = 0,
    }
end

local function start_animations(animations)
    stop_animation_timer()
    state.animations = animations or {}
    state.animating = #state.animations > 0

    if not state.animating then
        reset_all_tile_transforms()
        return
    end

    with_batch(function()
        for row = 1, MATRIX_SIZE do
            for col = 1, MATRIX_SIZE do
                reset_tile_transform(state.tiles[row][col])
            end
        end

        for index = 1, #state.animations do
            local anim = state.animations[index]
            local tile = state.tiles[anim.row][anim.col]
            if anim.kind == "spawn" then
                set_tile_transform(tile, 0, 0, anim.from_zoom, anim.from_opa)
            end
        end
    end)

    state.anim_timer = lv.timer_create(function(timer)
        local all_done = true

        with_batch(function()
            for index = 1, #state.animations do
                local anim = state.animations[index]
                local tile = state.tiles[anim.row][anim.col]
                anim.elapsed = math.min(anim.elapsed + ANIM_FRAME_MS, anim.duration)

                local progress = 1
                if anim.duration > 0 then
                    progress = clamp(anim.elapsed / anim.duration, 0, 1)
                end

                if anim.kind == "spawn" then
                    local eased = ease_out_back(progress)
                    local fade = ease_out_cubic(progress)
                    set_tile_transform(
                        tile,
                        0,
                        0,
                        lerp(anim.from_zoom, anim.to_zoom, eased),
                        lerp(anim.from_opa, anim.to_opa, fade)
                    )
                end

                if anim.elapsed < anim.duration then
                    all_done = false
                end
            end
        end)

        if all_done then
            lv.timer_del(timer)
            state.anim_timer = nil
            state.animations = {}
            state.animating = false
            update_board_ui()
        end
    end, ANIM_FRAME_MS)
end

local function set_overlay_visible(visible)
    if not state.overlay then
        return
    end

    if visible then
        lv.obj_clear_flag(state.overlay, lv.FLAG_HIDDEN)
    else
        lv.obj_add_flag(state.overlay, lv.FLAG_HIDDEN)
    end
end

update_board_ui = function()
    with_batch(function()
        for row = 1, MATRIX_SIZE do
            for col = 1, MATRIX_SIZE do
                local exp = state.board[row][col]
                local tile = state.tiles[row][col]
                local label = state.tile_labels[row][col]

                set_tile_empty_cover(tile, false)
                lv.obj_set_style_bg_color(tile, get_tile_color(exp), 0)
                lv.obj_set_style_text_color(tile, get_tile_text_color(exp), 0)
                lv.label_set_text(label, board_value_text(exp))
            end
        end

        lv.label_set_text(state.score_label, tostring(state.score))
        lv.label_set_text(state.best_label, tostring(get_best_tile(state.board)))

        if state.game_over then
            lv.label_set_text(state.status_label, "游戏结束")
            lv.label_set_text(state.overlay_label, "游戏结束\n点击新游戏重新开始")
            set_overlay_visible(not state.animating)
        else
            local best = get_best_tile(state.board)
            if best == 2048 then
                lv.label_set_text(state.status_label, "2048!")
            else
                lv.label_set_text(state.status_label, "游玩中")
            end
            set_overlay_visible(false)
        end
    end)
end

local function new_game()
    dbg("new_game")
    clear_animations()
    state.score = 0
    state.game_over = false
    state.board = {}

    for row = 1, MATRIX_SIZE do
        state.board[row] = {}
        for col = 1, MATRIX_SIZE do
            state.board[row][col] = 0
        end
    end

    local spawned = {}
    spawned[#spawned + 1] = add_random(state.board)
    spawned[#spawned + 1] = add_random(state.board)
    update_board_ui()
    local animations = {}
    for index = 1, #spawned do
        add_spawn_animation(animations, spawned[index])
    end
    start_animations(animations)
end

local function attempt_move(move_fn)
    if state.game_over or state.animating then
        dbg("attempt_move ignored: game_over")
        return
    end

    local success = move_fn(state.board)
    if not success then
        dbg("attempt_move no_change")
        return
    end

    local animations = {}
    dbg("attempt_move success score=", state.score)
    local spawned_tile = add_random(state.board)
    add_spawn_animation(animations, spawned_tile)
    state.animating = #animations > 0
    state.game_over = is_game_over(state.board)
    update_board_ui()
    start_animations(animations)
end

local function gesture_move(dir)
    if dir == lv.DIR_TOP then
        attempt_move(move_left)
    elseif dir == lv.DIR_BOTTOM then
        attempt_move(move_right)
    elseif dir == lv.DIR_LEFT then
        attempt_move(move_up)
    elseif dir == lv.DIR_RIGHT then
        attempt_move(move_down)
    end
end

local function read_event_point(e)
    local point = e and e.point or nil
    if point then
        dbg("event_point", point.x, point.y)
    else
        dbg("event_point nil")
    end
    return point
end

local function on_board_touch(e)
    local code = lv.event_get_code(e)
    dbg("touch_event code=", code)

    if code == lv.EVENT_PRESSED then
        local point = read_event_point(e)
        if point then
            state.touch_start = { x = point.x, y = point.y }
            dbg("touch_start", point.x, point.y)
        else
            dbg("touch_start missing_point")
        end
        return
    end

    if code ~= lv.EVENT_RELEASED then
        dbg("touch_event ignored")
        return
    end

    local start = state.touch_start
    state.touch_start = nil
    if not start then
        dbg("touch_release without_start")
        return
    end

    local point = read_event_point(e)
    if not point then
        dbg("touch_release missing_end_point")
        return
    end

    local dx = point.x - start.x
    local dy = point.y - start.y
    local adx = math.abs(dx)
    local ady = math.abs(dy)
    local threshold = 16

    dbg("touch_delta", "dx=", dx, "dy=", dy, "threshold=", threshold)

    if adx < threshold and ady < threshold then
        dbg("touch_delta below_threshold")
        return
    end

    if adx >= ady then
        if dx > 0 then
            dbg("gesture resolved RIGHT")
            gesture_move(lv.DIR_RIGHT)
        else
            dbg("gesture resolved LEFT")
            gesture_move(lv.DIR_LEFT)
        end
    else
        if dy > 0 then
            dbg("gesture resolved DOWN")
            gesture_move(lv.DIR_BOTTOM)
        else
            dbg("gesture resolved UP")
            gesture_move(lv.DIR_TOP)
        end
    end
end

lv.obj_set_style_bg_color(page, COLORS.bg, 0)
lv.obj_set_style_bg_grad_color(page, 0xF3EEE6, 0)
lv.obj_set_style_bg_grad_dir(page, lv.GRAD_DIR_VER, 0)
lv.obj_set_style_border_width(page, 0, 0)
lv.obj_clear_flag(page, lv.FLAG_SCROLLABLE)

state.status_label = lv.label_create(page)
lv.label_set_text(state.status_label, "进行中")
lv.obj_set_style_text_color(state.status_label, COLORS.text_dark, 0)
lv.obj_align(state.status_label, lv.ALIGN_TOP_LEFT, 10, 26)

local score_panel = lv.obj_create(page)
lv.obj_set_size(score_panel, 58, 34)
lv.obj_align(score_panel, lv.ALIGN_TOP_LEFT, 15, 48)
lv.obj_set_style_bg_color(score_panel, COLORS.panel, 0)
lv.obj_set_style_border_width(score_panel, 0, 0)
lv.obj_set_style_radius(score_panel, 8, 0)
lv.obj_set_style_pad_all(score_panel, 2, 0)

local score_title = lv.label_create(score_panel)
lv.label_set_text(score_title, "分数")
lv.obj_set_style_text_color(score_title, COLORS.text_light, 0)
-- lv.obj_set_style_text_font(score_title, lv.font_montserrat_10, 0)
lv.obj_align(score_title, lv.ALIGN_TOP_MID, 0, 0)

state.score_label = lv.label_create(score_panel)
lv.label_set_text(state.score_label, "0")
lv.obj_set_style_text_color(state.score_label, COLORS.text_light, 0)
-- lv.obj_set_style_text_font(state.score_label, lv.font_montserrat_16, 0)
lv.obj_align(state.score_label, lv.ALIGN_BOTTOM_MID, 0, -1)

local best_panel = lv.obj_create(page)
lv.obj_set_size(best_panel, 58, 34)
lv.obj_align(best_panel, lv.ALIGN_TOP_LEFT, 15, 88)
lv.obj_set_style_bg_color(best_panel, COLORS.panel, 0)
lv.obj_set_style_border_width(best_panel, 0, 0)
lv.obj_set_style_radius(best_panel, 8, 0)
lv.obj_set_style_pad_all(best_panel, 2, 0)

local best_title = lv.label_create(best_panel)
lv.label_set_text(best_title, "最大数")
lv.obj_set_style_text_color(best_title, COLORS.text_light, 0)
-- lv.obj_set_style_text_font(best_title, lv.font_montserrat_10, 0)
lv.obj_align(best_title, lv.ALIGN_TOP_MID, 0, 0)

state.best_label = lv.label_create(best_panel)
lv.label_set_text(state.best_label, "0")
lv.obj_set_style_text_color(state.best_label, COLORS.text_light, 0)
-- lv.obj_set_style_text_font(state.best_label, lv.font_montserrat_16, 0)
lv.obj_align(state.best_label, lv.ALIGN_BOTTOM_MID, 0, -1)

local restart_btn = lv.btn_create(page)
lv.obj_set_size(restart_btn, 72, 26)
lv.obj_align(restart_btn, lv.ALIGN_TOP_LEFT, 15, 128)
lv.obj_set_style_bg_color(restart_btn, COLORS.restart, 0)
lv.obj_set_style_border_width(restart_btn, 0, 0)
lv.obj_set_style_radius(restart_btn, 8, 0)
lv.obj_set_style_shadow_width(restart_btn, 0, 0)

local restart_label = lv.label_create(restart_btn)
lv.label_set_text(restart_label, "新游戏")
lv.obj_set_style_text_color(restart_label, COLORS.text_light, 0)
lv.obj_center(restart_label)

local board_panel = lv.obj_create(page)
lv.obj_set_size(board_panel, BOARD_SIZE, BOARD_SIZE)
lv.obj_align(board_panel, lv.ALIGN_BOTTOM_MID, BOARD_X_OFFSET, -8)
lv.obj_set_style_bg_color(board_panel, COLORS.panel, 0)
lv.obj_set_style_border_width(board_panel, 0, 0)
lv.obj_set_style_radius(board_panel, 12, 0)
lv.obj_set_style_pad_all(board_panel, BOARD_PAD, 0)
lv.obj_set_style_pad_gap(board_panel, TILE_GAP, 0)
lv.obj_set_flex_flow(board_panel, lv.FLEX_FLOW_ROW_WRAP)
lv.obj_set_style_shadow_width(board_panel, 8, 0)
lv.obj_set_style_shadow_color(board_panel, COLORS.panel_shadow, 0)
lv.obj_set_style_shadow_opa(board_panel, lv.OPA_COVER, 0)
lv.obj_set_style_shadow_ofs_y(board_panel, 2, 0)

for row = 1, MATRIX_SIZE do
    state.tiles[row] = {}
    state.tile_labels[row] = {}
    for col = 1, MATRIX_SIZE do
        local tile = lv.obj_create(board_panel)
        lv.obj_set_size(tile, TILE_SIZE, TILE_SIZE)
        lv.obj_set_style_bg_color(tile, COLORS.tile_empty, 0)
        lv.obj_set_style_border_width(tile, 0, 0)
        lv.obj_set_style_radius(tile, 6, 0)
        lv.obj_set_style_shadow_width(tile, 0, 0)
        lv.obj_set_style_transform_pivot_x(tile, math.floor(TILE_SIZE / 2), 0)
        lv.obj_set_style_transform_pivot_y(tile, math.floor(TILE_SIZE / 2), 0)
        lv.obj_set_style_transform_zoom(tile, TILE_ZOOM_DEFAULT, 0)
        lv.obj_clear_flag(tile, lv.FLAG_SCROLLABLE)
        lv.obj_add_flag(tile, lv.FLAG_CLICKABLE)

        local label = lv.label_create(tile)
        lv.label_set_text(label, "")
        -- lv.obj_set_style_text_font(label, lv.font_montserrat_18, 0)
        lv.obj_center(label)

        lv.obj_add_event_cb(tile, on_board_touch, lv.EVENT_PRESSED)
        lv.obj_add_event_cb(tile, on_board_touch, lv.EVENT_RELEASED)

        state.tiles[row][col] = tile
        state.tile_labels[row][col] = label
    end
end

state.overlay = lv.obj_create(board_panel)
lv.obj_set_size(state.overlay, BOARD_SIZE - 16, BOARD_SIZE - 16)
lv.obj_add_flag(state.overlay, lv.FLAG_IGNORE_LAYOUT)
lv.obj_align(state.overlay, lv.ALIGN_CENTER, 0, 0)
lv.obj_set_style_bg_color(state.overlay, 0xF2E9DC, 0)
lv.obj_set_style_bg_opa(state.overlay, 220, 0)
lv.obj_set_style_border_width(state.overlay, 0, 0)
lv.obj_set_style_radius(state.overlay, 10, 0)

state.overlay_label = lv.label_create(state.overlay)
lv.label_set_text(state.overlay_label, "游戏结束")
lv.obj_set_style_text_color(state.overlay_label, COLORS.text_dark, 0)
-- lv.obj_set_style_text_font(state.overlay_label, lv.font_montserrat_22, 0)
lv.obj_set_style_text_align(state.overlay_label, lv.TEXT_ALIGN_CENTER, 0)
lv.obj_center(state.overlay_label)
lv.obj_add_flag(state.overlay, lv.FLAG_HIDDEN)
lv.obj_add_flag(state.overlay, lv.FLAG_CLICKABLE)
lv.obj_add_event_cb(state.overlay, on_board_touch, lv.EVENT_PRESSED)
lv.obj_add_event_cb(state.overlay, on_board_touch, lv.EVENT_RELEASED)

lv.obj_add_event_cb(restart_btn, function(e)
    if lv.event_get_code(e) == lv.EVENT_CLICKED then
        new_game()
    end
end, lv.EVENT_CLICKED)

lv.obj_add_flag(board_panel, lv.FLAG_CLICKABLE)
lv.obj_add_event_cb(board_panel, on_board_touch, lv.EVENT_PRESSED)
lv.obj_add_event_cb(board_panel, on_board_touch, lv.EVENT_RELEASED)

lv.obj_add_event_cb(page, function(e)
    if lv.event_get_code(e) == lv.EVENT_SCREEN_UNLOAD_START then
        clear_animations()
        collectgarbage("collect")
    end
end, lv.EVENT_SCREEN_UNLOAD_START)

new_game()

sys.timerLoopStart(function()
    lv.poll_events(10)
end, 16)

sys.run()
