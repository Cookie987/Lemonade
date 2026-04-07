-- Calculator
-- com.lemonade.calculator

local sys = require("sys")
local lv = lvgl

local page = lv.app_page()
if not page then return end

lv.obj_clean(page)

local font = lv.font_load("assets/jetbrains_mono_bold_32.bin")
if not font then
    ui.show_notification("字体加载失败", "", 2000)
    return
end

local DEG_TO_RAD = math.pi / 180
local RAD_TO_DEG = 180 / math.pi
local DISPLAY_LIMIT = 24

local expression = "0"
local last_result = "0"
local angle_mode = "DEG"
local has_error = false
local clear_btn_label = nil

local function with_batch(fn)
    lv.batch_begin()
    local ok, result = pcall(fn)
    lv.batch_end()
    if not ok then
        error(result)
    end
    return result
end

lv.obj_set_style_bg_color(page, 0xFFFFFF, 0)

local display_panel = lv.obj_create(page)
lv.obj_set_size(display_panel, 320, 68)
lv.obj_align(display_panel, lv.ALIGN_TOP_MID, 0, 0)
lv.obj_set_style_bg_opa(display_panel, 0, 0)
lv.obj_set_style_border_width(display_panel, 0, 0)
lv.obj_set_style_pad_all(display_panel, 0, 0)

local mode_label = lv.label_create(display_panel)
lv.obj_align(mode_label, lv.ALIGN_TOP_LEFT, 12, 15)

local display_label = lv.label_create(display_panel)
lv.obj_set_style_text_font(display_label, font, 0)
lv.obj_set_width(display_label, 300)
lv.obj_set_style_text_align(display_label, lv.TEXT_ALIGN_RIGHT, 0)
lv.obj_align(display_label, lv.ALIGN_BOTTOM_RIGHT, -10, -8)

local hint_label = lv.label_create(display_panel)
lv.obj_set_width(hint_label, 300)
lv.obj_set_style_text_align(hint_label, lv.TEXT_ALIGN_RIGHT, 0)
lv.obj_align(hint_label, lv.ALIGN_TOP_RIGHT, -10, 15)

local btn_grid = lv.obj_create(page)
lv.obj_set_size(btn_grid, 320, 172)
lv.obj_align(btn_grid, lv.ALIGN_BOTTOM_MID, 0, 0)
lv.obj_set_flex_flow(btn_grid, lv.FLEX_FLOW_ROW_WRAP)
lv.obj_set_style_pad_all(btn_grid, 4, 0)
lv.obj_set_style_pad_row(btn_grid, 4, 0)
lv.obj_set_style_pad_column(btn_grid, 4, 0)
lv.obj_set_style_bg_opa(btn_grid, 0, 0)
lv.obj_set_style_border_width(btn_grid, 0, 0)

local function trim_expression(text)
    if #text <= DISPLAY_LIMIT then
        return text
    end
    return "..." .. string.sub(text, #text - DISPLAY_LIMIT + 4)
end

local function update_display()
    with_batch(function()
        lv.label_set_text(mode_label, angle_mode)
        lv.label_set_text(hint_label, "Ans = " .. last_result)
        lv.label_set_text(display_label, trim_expression(expression))
        if clear_btn_label then
            local clear_text = (expression == "0" and not has_error) and "AC" or "C"
            lv.label_set_text(clear_btn_label, clear_text)
        end
    end)
end

local function is_digit(ch)
    return ch and ch:match("%d") ~= nil
end

local function ends_with_number_or_close(text)
    local last = string.sub(text, -1)
    return is_digit(last) or last == "." or last == ")" or last == "i" or last == "s" or last == "e"
end

local function should_insert_multiply_before(fragment)
    if expression == "0" then
        return false
    end
    if not ends_with_number_or_close(expression) then
        return false
    end
    return fragment == "("
        or fragment == "pi"
        or fragment == "e"
        or fragment == "ans"
        or fragment == "sin("
        or fragment == "cos("
        or fragment == "tan("
        or fragment == "asin("
        or fragment == "acos("
        or fragment == "atan("
        or fragment == "sqrt("
        or fragment == "ln("
        or fragment == "log("
        or fragment == "abs("
end

local function append_fragment(fragment)
    if fragment == nil or fragment == "" then
        return
    end

    if has_error then
        expression = "0"
        has_error = false
    end

    if should_insert_multiply_before(fragment) then
        expression = expression .. "*"
    end

    if expression == "0" and fragment ~= "." then
        if fragment == "(" or fragment == "pi" or fragment == "e" or fragment == "ans" then
            expression = fragment
        elseif string.match(fragment, "^%a") then
            expression = fragment
        else
            expression = fragment
        end
    else
        expression = expression .. fragment
    end
end

local function append_digit(digit)
    if has_error then
        expression = digit
        has_error = false
        return
    end
    if string.sub(expression, -1) == ")" or string.sub(expression, -1):match("[%a]") then
        expression = expression .. "*" .. digit
        return
    end
    if expression == "0" then
        expression = digit
    else
        expression = expression .. digit
    end
end

local function append_decimal()
    if has_error then
        expression = "0."
        has_error = false
        return
    end

    local i = #expression
    while i > 0 do
        local ch = string.sub(expression, i, i)
        if ch == "." then
            return
        end
        if not is_digit(ch) then
            break
        end
        i = i - 1
    end

    local last = string.sub(expression, -1)
    if last == ")" or last:match("[%a]") then
        append_fragment("0.")
    elseif expression == "0" or not is_digit(last) then
        append_fragment("0.")
    else
        expression = expression .. "."
    end
end

local function backspace()
    if has_error or #expression <= 1 then
        expression = "0"
        has_error = false
        return
    end
    expression = string.sub(expression, 1, #expression - 1)
    if expression == "" or expression == "-" then
        expression = "0"
    end
end

local function clear_all()
    expression = "0"
    has_error = false
end

local function clear_all_and_ans()
    expression = "0"
    last_result = "0"
    has_error = false
end

local function tokenize(text)
    local tokens = {}
    local i = 1
    local previous = nil

    while i <= #text do
        local ch = string.sub(text, i, i)

        if ch == " " then
            i = i + 1
        elseif ch:match("%d") or ch == "." then
            local start_idx = i
            i = i + 1
            while i <= #text and string.sub(text, i, i):match("[%d%.]") do
                i = i + 1
            end
            local number_text = string.sub(text, start_idx, i - 1)
            local number_value = tonumber(number_text)
            if not number_value then
                error("Invalid number")
            end
            previous = { kind = "number", value = number_value }
            table.insert(tokens, previous)
        elseif ch:match("[%a_]") then
            local start_idx = i
            i = i + 1
            while i <= #text and string.sub(text, i, i):match("[%a_]") do
                i = i + 1
            end
            local name = string.sub(text, start_idx, i - 1)
            if name == "pi" or name == "e" or name == "ans" then
                previous = { kind = "const", value = name }
            else
                previous = { kind = "func", value = name }
            end
            table.insert(tokens, previous)
        elseif ch == "(" or ch == ")" then
            previous = { kind = "paren", value = ch }
            table.insert(tokens, previous)
            i = i + 1
        elseif ch == "+" or ch == "*" or ch == "/" or ch == "^" then
            previous = { kind = "op", value = ch }
            table.insert(tokens, previous)
            i = i + 1
        elseif ch == "-" then
            local unary = previous == nil
                or (previous.kind == "op")
                or (previous.kind == "paren" and previous.value == "(")
            previous = { kind = "op", value = unary and "u-" or "-" }
            table.insert(tokens, previous)
            i = i + 1
        else
            error("Syntax error: " .. ch)
        end
    end

    return tokens
end

local operator_info = {
    ["+"] = { precedence = 1, assoc = "left", argc = 2 },
    ["-"] = { precedence = 1, assoc = "left", argc = 2 },
    ["*"] = { precedence = 2, assoc = "left", argc = 2 },
    ["/"] = { precedence = 2, assoc = "left", argc = 2 },
    ["^"] = { precedence = 4, assoc = "right", argc = 2 },
    ["u-"] = { precedence = 3, assoc = "right", argc = 1 },
}

local function to_rpn(tokens)
    local output = {}
    local stack = {}

    for _, token in ipairs(tokens) do
        if token.kind == "number" or token.kind == "const" then
            table.insert(output, token)
        elseif token.kind == "func" then
            table.insert(stack, token)
        elseif token.kind == "op" then
            local current = operator_info[token.value]
            while #stack > 0 do
                local top = stack[#stack]
                if top.kind == "func" then
                    table.insert(output, table.remove(stack))
                elseif top.kind == "op" then
                    local top_info = operator_info[top.value]
                    local should_pop = top_info
                        and (
                            (current.assoc == "left" and current.precedence <= top_info.precedence)
                            or (current.assoc == "right" and current.precedence < top_info.precedence)
                        )
                    if should_pop then
                        table.insert(output, table.remove(stack))
                    else
                        break
                    end
                else
                    break
                end
            end
            table.insert(stack, token)
        elseif token.kind == "paren" and token.value == "(" then
            table.insert(stack, token)
        elseif token.kind == "paren" and token.value == ")" then
            local matched = false
            while #stack > 0 do
                local top = table.remove(stack)
                if top.kind == "paren" and top.value == "(" then
                    matched = true
                    break
                end
                table.insert(output, top)
            end
            if not matched then
                error("Syntax error")
            end
            if #stack > 0 and stack[#stack].kind == "func" then
                table.insert(output, table.remove(stack))
            end
        end
    end

    while #stack > 0 do
        local top = table.remove(stack)
        if top.kind == "paren" then
            error("Syntax error")
        end
        table.insert(output, top)
    end

    return output
end

local function make_trig_function(name)
    local fn = math[name]
    return function(value)
        return fn(angle_mode == "DEG" and value * DEG_TO_RAD or value)
    end
end

local function make_inverse_trig_function(name)
    local fn = math[name]
    return function(value)
        local result = fn(value)
        return angle_mode == "DEG" and result * RAD_TO_DEG or result
    end
end

local scientific_functions = {
    sin = make_trig_function("sin"),
    cos = make_trig_function("cos"),
    tan = make_trig_function("tan"),
    asin = make_inverse_trig_function("asin"),
    acos = make_inverse_trig_function("acos"),
    atan = make_inverse_trig_function("atan"),
    sqrt = math.sqrt,
    abs = math.abs,
    ln = math.log,
    log = function(value)
        return math.log(value, 10)
    end,
}

local scientific_constants = {
    pi = math.pi,
    e = math.exp(1),
    ans = function()
        return tonumber(last_result) or 0
    end,
}

local function resolve_constant(name)
    local constant = scientific_constants[name]
    if type(constant) == "function" then
        return constant()
    end
    return constant
end

local function evaluate_rpn(rpn)
    local stack = {}

    for _, token in ipairs(rpn) do
        if token.kind == "number" then
            table.insert(stack, token.value)
        elseif token.kind == "const" then
            local value = resolve_constant(token.value)
            if value == nil then
                error("No const: " .. token.value)
            end
            table.insert(stack, value)
        elseif token.kind == "func" then
            local fn = scientific_functions[token.value]
            if not fn then
                error("" .. token.value)
            end
            local operand = table.remove(stack)
            if operand == nil then
                error("Arg missing")
            end
            local result = fn(operand)
            if result ~= result or result == math.huge or result == -math.huge then
                error("Math error")
            end
            table.insert(stack, result)
        elseif token.kind == "op" then
            local info = operator_info[token.value]
            if info.argc == 1 then
                local operand = table.remove(stack)
                if operand == nil then
                    error("Exp Err")
                end
                table.insert(stack, -operand)
            else
                local right = table.remove(stack)
                local left = table.remove(stack)
                if left == nil or right == nil then
                    error("Exp Err")
                end
                local result
                if token.value == "+" then
                    result = left + right
                elseif token.value == "-" then
                    result = left - right
                elseif token.value == "*" then
                    result = left * right
                elseif token.value == "/" then
                    if right == 0 then
                        error("Math error")
                    end
                    result = left / right
                elseif token.value == "^" then
                    result = left ^ right
                end
                if result ~= result or result == math.huge or result == -math.huge then
                    error("Math error")
                end
                table.insert(stack, result)
            end
        end
    end

    if #stack ~= 1 then
        error("Exp Err")
    end
    return stack[1]
end

local function format_result(value)
    local rounded = math.floor(value)
    if math.abs(value - rounded) < 1e-10 then
        return tostring(rounded)
    end

    local text = string.format("%.10f", value)
    text = string.gsub(text, "0+$", "")
    text = string.gsub(text, "%.$", "")
    return text
end

local function calculate_expression()
    local ok, result = pcall(function()
        local tokens = tokenize(expression)
        local rpn = to_rpn(tokens)
        return evaluate_rpn(rpn)
    end)

    if ok then
        last_result = format_result(result)
        expression = last_result
        has_error = false
    else
        expression = tostring(result)
        has_error = true
    end
end

local function toggle_angle_mode()
    angle_mode = angle_mode == "DEG" and "RAD" or "DEG"
end

local function on_btn_click(e)
    local btn = lv.event_get_target(e)
    local label = lv.obj_get_child(btn, 0)
    local txt = lv.label_get_text(label)

    if txt:match("^%d$") then
        append_digit(txt)
    elseif txt == "." then
        append_decimal()
    elseif txt == "C" or txt == "AC" then
        if txt == "AC" then
            clear_all_and_ans()
        else
            clear_all()
        end
    elseif txt == "DEL" then
        backspace()
    elseif txt == "=" then
        calculate_expression()
    elseif txt == "DEG" or txt == "RAD" then
        toggle_angle_mode()
    elseif txt == "pi" or txt == "e" or txt == "ans" then
        append_fragment(txt)
    elseif txt == "sin" or txt == "cos" or txt == "tan"
        or txt == "asin" or txt == "acos" or txt == "atan"
        or txt == "sqrt" or txt == "ln" or txt == "log" or txt == "abs" then
        append_fragment(txt .. "(")
    else
        append_fragment(txt)
    end

    update_display()
end

local buttons = {
    "DEG", "(", ")", "DEL", "AC",
    "sin", "cos", "tan", "asin", "acos",
    "ln", "log", "sqrt", "abs", "^",
    "7", "8", "9", "pi", "/",
    "4", "5", "6", "e", "*",
    "1", "2", "3", "ans", "-",
    "0", ".", "atan", "+", "=",
}

with_batch(function()
    for _, txt in ipairs(buttons) do
        local btn = lv.btn_create(btn_grid)
        lv.obj_set_size(btn, 58, 20)

        local lbl = lv.label_create(btn)
        lv.label_set_text(lbl, txt)
        lv.obj_center(lbl)

        if txt == "AC" then
            clear_btn_label = lbl
        end

        if txt == "AC" or txt == "DEL" then
            lv.obj_set_style_bg_color(btn, 0xC0392B, 0)
        elseif txt == "DEG" or txt == "RAD" or txt == "=" then
            lv.obj_set_style_bg_color(btn, 0x2E86C1, 0)
        elseif txt == "/" or txt == "*" or txt == "-" or txt == "+" or txt == "^" then
            lv.obj_set_style_bg_color(btn, 0x333333, 0)
        elseif txt == "sin" or txt == "cos" or txt == "tan"
            or txt == "asin" or txt == "acos" or txt == "atan"
            or txt == "sqrt" or txt == "ln" or txt == "log" or txt == "abs"
            or txt == "pi" or txt == "e" or txt == "ans" then
            lv.obj_set_style_bg_color(btn, 0x566573, 0)
        else
            lv.obj_set_style_bg_color(btn, 0x1A1A1A, 0)
        end

        lv.obj_add_event_cb(btn, on_btn_click, lv.EVENT_CLICKED)
    end
end)

update_display()

sys.timerLoopStart(function()
    lv.poll_events(10)
end, 16)

sys.run()
