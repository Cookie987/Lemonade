-- 废墟图书馆 - Prescript

local sys = require("sys")

local lv = lvgl
local page = lv.app_page()
if not page then
    log.error("app_page is nil")
    return
end

local unifont_24 = lv.font_load("assets/unifont_24.bin")

-- 背景
lv.obj_set_style_bg_color(page, 0x000000, 0)
lv.obj_set_style_bg_opa(page, lv.OPA_COVER, 0)

-- 主循环
sys.timerLoopStart(function()
    lvgl.poll_events(50)  -- 阻塞最多 50ms
end, 150)

sys.run()
