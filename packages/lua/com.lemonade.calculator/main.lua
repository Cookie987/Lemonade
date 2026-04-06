-- 计算器 Calculator
-- com.lemonade.calculator

local sys = require("sys")
local lv = lvgl

local page = lv.app_page()
if not page then return end

-- 加载像素字体
-- local pixel_font = lv.font_load("assets/unifont_24.bin")

-- --- 内部状态 ---
local current_input = "0"
local first_operand = nil
local last_operator = nil
local should_reset = false

-- --- UI 组件 ---
lv.obj_set_style_bg_color(page, 0x000000, 0)

-- 显示屏
local display_label = lv.label_create(page)
-- lv.obj_set_style_text_font(display_label, pixel_font, 0)
lv.obj_set_width(display_label, 300)
lv.obj_set_style_text_align(display_label, lv.TEXT_ALIGN_RIGHT, 0)
lv.obj_align(display_label, lv.ALIGN_TOP_MID, 0, 40)
lv.label_set_text(display_label, current_input)

-- 按键容器 (使用 Flex 布局)
local btn_grid = lv.obj_create(page)
lv.obj_set_size(btn_grid, 320, 180)
lv.obj_align(btn_grid, lv.ALIGN_BOTTOM_MID, 0, 0)
lv.obj_set_flex_flow(btn_grid, lv.FLEX_FLOW_ROW_WRAP)
lv.obj_set_style_pad_all(btn_grid, 5, 0)
lv.obj_set_style_bg_opa(btn_grid, 0, 0) -- 背景透明
lv.obj_set_style_border_width(btn_grid, 0, 0)

-- --- 逻辑处理 ---
local function update_display()
    -- 限制长度防止溢出
    if #current_input > 12 then
        current_input = string.sub(current_input, 1, 12)
    end
    lv.label_set_text(display_label, current_input)
end

local function calculate()
    local second_operand = tonumber(current_input)
    if not first_operand or not last_operator then return end
    
    local result
    if last_operator == "+" then result = first_operand + second_operand
    elseif last_operator == "-" then result = first_operand - second_operand
    elseif last_operator == "*" then result = first_operand * second_operand
    elseif last_operator == "/" then 
        result = (second_operand == 0) and "Error" or (first_operand / second_operand)
    end
    
    current_input = tostring(result)
    first_operand = nil
    last_operator = nil
    should_reset = true
    update_display()
end

-- --- 按键回调 (使用你反转后的绑定顺序) ---
local function on_btn_click(e)
    local btn = lv.event_get_target(e)
    local label = lv.obj_get_child(btn, 0)
    local txt = lv.label_get_text(label)

    if tonumber(txt) or txt == "." then
        if should_reset then
            current_input = txt
            should_reset = false
        else
            if current_input == "0" and txt ~= "." then
                current_input = txt
            else
                current_input = current_input .. txt
            end
        end
    elseif txt == "C" then
        current_input = "0"
        first_operand = nil
        last_operator = nil
    elseif txt == "=" then
        calculate()
    else
        -- 运算符
        first_operand = tonumber(current_input)
        last_operator = txt
        should_reset = true
    end
    update_display()
end

-- --- 创建按键 ---
local buttons = {
    "7", "8", "9", "/",
    "4", "5", "6", "*",
    "1", "2", "3", "-",
    "C", "0", "=", "+"
}

for _, txt in ipairs(buttons) do
    local btn = lv.btn_create(btn_grid)
    lv.obj_set_size(btn, 70, 38) -- 适配 320 宽度的网格

    local lbl = lv.label_create(btn)
    lv.label_set_text(lbl, txt)
    lv.obj_center(lbl)

    -- 根据功能区分颜色
    if txt == "=" or txt == "C" then
        lv.obj_set_style_bg_color(btn, 0x15dcff, 0) -- 你的标志性青蓝色
    elseif txt == "/" or txt == "*" or txt == "-" or txt == "+" then
        lv.obj_set_style_bg_color(btn, 0x333333, 0)
    else
        lv.obj_set_style_bg_color(btn, 0x1A1A1A, 0)
    end

    lv.obj_add_event_cb(btn, on_btn_click, lv.EVENT_CLICKED)
end

-- 主循环
sys.timerLoopStart(function()
    lv.poll_events(10)
end, 150)

sys.run()