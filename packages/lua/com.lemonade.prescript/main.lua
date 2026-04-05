-- 废墟图书馆 - Prescript

local sys = require("sys")
local lv = lvgl

local page = lv.app_page()
if not page then return end

local unifont_24 = lv.font_load("assets/unifont_24.bin")
local mask_chars = "$%&*@?!<>/ "
local speed_ms = 80
local reveal_every = 4
local idle_text = "#15dcff - Click to receive -#"
local active_timer = nil

-- 数据池
local egg = {
    "向你最喜欢的女生表白，直到她“同意”或者“拒绝”为止",
    "去往任意一家便利店的厕所捅自己两刀，然后若无其事的走出来",
    "给星期四第1个问你要疯狂星期四vo50的人v他50",
    "请在两分钟内对着镜子说800遍「我是正常人」",
    "请在一天内睡够800个小时",
    "喝一升热牛奶，并开始剧烈运动",
    "请只用右手写字吃饭和扣下扳机",
    "给你今天第1个登录的游戏充值任意金额",
    "请在30分钟内找到伴侣，棕色头发最佳",
    "在90小时内找到自己感觉舒适的人杀死，并把内脏挂在自己家的墙上",
    "请在一小时内去往任意图书馆拿出一本书，并且不让任何人发现",
    "走到大街上切断自己的一根拇指，并吃掉它",
    "戴上黑色的面具在大街上找到任意人并对其说苦痛就是我的唯一",
    "今天内睡10分钟，醒10分钟，重复24小时",
    "举起一把刀，刺伤一个熟悉又温暖的身体",
    "拿起剪刀剪断木偶线，让自己重获自由",
    "请在睡觉前眨三次眼，并点一次头",
    "请在一小时内找到任意白色液体，并喝掉",
    "出门见到的第1个自己最熟悉的人，并给其一拳",
    "请从高空跃下，并且不做一切安全措施",
    "请 在 念 完 自 然 常 数 e 之 前 不 要 归 家",
    "请砍下自己的右脚并吃掉（骨头不用）",
    "请在一天之内工作12个小时后立即下班",
    "请前往B站为你见到的第1个视频点赞",
    "请上两元的公交车，并只给一块钱",
    "请在最痛苦时开怀大笑",
    "请在4小时内听完所有自己最喜欢的音乐",
    "请在一天内生病20次",
    "请在24小时内读完π的所有数",
    "用橡皮当笔，并写完一次作业",
    "--蓄力猛击自己魔丸9178次",
    "请在自己生日当天的凌晨0点00分准时祝自己生日快乐",
    "...请用叉子当筷子吃饭",
    "找到一只山羊，并骑在上面两小时",
    "一直盯着镜子里的自己看，直到自己开始不认识自己",
    "假装自己是明星，并让任意一个人向你寻求签名",
    "...请在一小时内练会舞蹈",
    "假装自己是警察，并对任意一个人说你被逮捕了",
    "在大街上买一桶爆米花并且爆米花的数量为56",
    "请在24小时内模仿中端机",
    "...请在空调房吹一小时冷气",
    "打开任意自己最喜欢的游戏，并玩24小时...",
    "假装自己是维特鲁姆人，并从高空跃下",
    "找到一首自己最喜欢的哈基米音乐，并将其唱完",
    "请在睡着后说完26位英文字母",
    "今天模仿猫咪让猫咪睡床，你睡猫窝",
    "如果你是左撇子，请砍掉右手",
    "请将右手砍下，并且不要用左手写字",
    "请在24小时内喝下一升的人血",
    "请买一个盗版的任意东西，并以正版的价格卖给别人",
    "请在90小时内写一部小说，并让自己出名",
    "请在一天内cos自己最喜欢的角色",
    "来到漆黑的下水道，并念出痛苦啊你便是我的唯一...",
    "请在今天内站在垃圾桶上享用晚餐...",
    "在24小时内站在大街上淋一场雨",
    "假装自己是女人，并和一位男生交往",
    "前往都市中心拆散一对同，并警告以后不能再这么做",
    "走到大街上见到的第1个女生，并亲吻她",
    "请在星期一给自己放一次24小时的假",
    "请在朋友未察觉的情况下借他200元",
    "请在24小时内，让自己最喜欢的人发现自己最奇怪的性癖",
    "在24小时内看完自己所有最喜欢的动漫",
    "请在一小时内学会游泳，并前往湖里试炼",
    "请在三小时内买到免费的鸡蛋",
    "请和一位游戏天才PK并获胜",
    "请在一天内，心脏跳动500下",
    "如你感觉自己被约束，请让自己重获自由",
    "请在24小时内欺骗朋友，并让他相信这是真的",
    "请在晚上9点向星星许愿，并在接下来的两小时内完成愿望",
    "在24小时内成为消防员，并救助一场火灾",
    "请在24小时内找到自己最喜欢的人，并一直表白到其同意",
    "找到你的朋友，并展示你的后空翻",
    "如你是左撇子，请以后主要使用右手，如你是右撇子，请以后主要使用左手",
    "前往医院，把左手装到右手，右手装到左手",
    "请在一分钟之内找到视频时长为11分45秒的视频",
    "点一份外卖，如超时请将外卖员殴打一顿",
    "煮一碗面，并只吃一半的面条",
    "请前往医院，并死活跟医生说我有病",
    "在“冬天”开空调，“夏天”开暖气",
    "制作一份便当并在街道上的任意垃圾桶旁品尝",
    "请在晚上晒日光浴...",
    "请在装满热牛奶的泳池里跳舞，并把牛奶全喝了",
    "…请在白色的墙上看到绿色",
    "在写作业时听都市之子，并且发三条弹幕，点赞三连",
    "聆听自我，然后前往你所想前往的地方",
    "请在当前或之后看的第1个视频将其评论全部看完",
    "和朋友吵架，直到朋友不回怼为止",
    "打开边狱巴士，并只用一次通关15牢",
    "…在两小时内刷完一套高考模拟试题",
    "···在今天向你暗恋的人表白，并向右跳一下"
}

local scn = {
    "请在两分钟内",
    "请在一天的时间里",
    "在九十小时内",
    "请在一小时内",
    "今天之内必须",
    "请在睡前",
    "请从现在开始",
    "请在二十四小时内",
    "请在三十分钟内",
    "请立刻打开",
    "请在四小时内",
    "用你最顺手的方式",
    "请在自己生日当天的凌晨零点",
    "在大街上当众",
    "请在空调房里",
    "打开你最喜欢的游戏并",
    "找到一首哈基米音乐并",
    "如果你是左撇子请",
    "请将手边的物品",
    "请在三小时内",
    "请和一位游戏天才一起",
    "若你感觉被约束请",
    "请在晚上九点整",
    "在二十四小时内务必",
    "左撇子请从此之后",
    "请在一分钟之内",
    "点一份外卖若超时就",
    "煮一碗热面并",
    "请前往附近的医院并",
    "在冬天的户外",
    "在夏天的烈日下",
    "制作一份便当并",
    "请在深夜里",
    "在装满热牛奶的泳池中",
    "写作业的过程中",
    "在你看的下一个视频里",
    "和你的好朋友一起",
    "打开边狱巴士并",
    "请在两小时内",
    "请在今天结束前",
    "前往B站为你刷到的第一个视频"
}

local act = {
    "向你最喜欢的女生表白",
    "对着镜子说八百遍我是正常人",
    "喝下一升的热牛奶",
    "开始三十分钟的剧烈运动",
    "只用右手写字吃饭和做所有事",
    "给今天第一个登录的游戏充值任意金额",
    "把自己的内脏挂在家中的墙上",
    "去往图书馆拿出一本任意的书",
    "戴上黑色面具找到一个陌生人",
    "对他说苦痛就是我的唯一",
    "睡十分钟醒十分钟重复二十四小时",
    "举起一把刀对着墙面划下痕迹",
    "眨三次眼并用力点一次头",
    "找到任意白色液体并喝掉",
    "给见到的第一个熟人一拳",
    "不念完自然常数e就不要回家",
    "工作十二小时后立刻下班休息",
    "在最痛苦的时候开怀大笑",
    "听完所有你最喜欢的音乐",
    "用橡皮当笔写完一次作业",
    "准时祝自己生日快乐",
    "用叉子当筷子吃完一顿饭",
    "找到一只山羊并骑在上面两小时",
    "盯着镜子里的自己直到认不出",
    "假装自己是明星并让路人找你签名",
    "学会一支简单的舞蹈并跳出来",
    "假装自己是警察并对路人说你被逮捕了",
    "买五十六桶爆米花并全部打开",
    "吹一小时的冷气不许离开",
    "连续玩二十四小时的游戏不休息",
    "把这首哈基米音乐完整唱完",
    "流利说完二十六个英文字母",
    "模仿猫咪的样子让猫睡床你睡猫窝",
    "喝下一升的温牛奶",
    "买一个盗版物品并以正版价格卖出",
    "写一篇短篇小说并分享给三个人",
    "cos你最喜欢的游戏角色出门",
    "在漆黑的下水道念出苦痛是我的唯一",
    "站在垃圾桶上享用你的晚餐",
    "站在大街上淋一场完整的雨",
    "亲吻你见到的第一个女生",
    "给自己放二十四小时的无理由假期",
    "在朋友未察觉时借给他两百元",
    "让喜欢的人发现你最奇怪的小癖好",
    "看完所有你最喜欢的动漫",
    "学会游泳并前往湖边尝试",
    "免费拿到一颗鸡蛋并好好保存",
    "和朋友PK并一定要获得胜利",
    "向星星许愿并两小时内完成愿望",
    "成为消防员并参与一次火灾救助",
    "向暗恋的人表白并向右跳一下",
    "展示一个你最拿手的后空翻",
    "把左手和右手的物品互换使用",
    "找到一个时长11分45秒的视频并看完",
    "只吃一半煮好的面条剩下的倒掉",
    "跟医生坚持说自己有病并说出症状",
    "打开空调调到最低温度",
    "打开暖气调到最高温度",
    "在街道垃圾桶旁品尝你的零食",
    "晒三十分钟的日光浴不遮挡",
    "跳一支舞并把泳池里的牛奶喝完",
    "在白墙上看出绿色的图案",
    "听都市之子并发布三条弹幕点赞三连",
    "聆听自己的内心并前往想去的地方",
    "把这个视频的评论全部看完",
    "和朋友吵架直到他不再回怼",
    "只用一次通关边狱巴士15牢",
    "刷完一套高考模拟试题并核对答案"
}

local sup = {
    "直到她明确同意或者拒绝为止",
    "然后若无其事的离开现场",
    "选择棕色头发的人最佳",
    "并且全程不让任何人发现",
    "骨头部分可以不用处理",
    "全程保持面无表情不说话",
    "并且只给外卖员一块钱配送费",
    "做完之后抬头数十分钟的星星",
    "优先选择棕色头发的陌生人",
    "做这件事时无需有任何犹豫",
    "完成后一定要拍照留念记录",
    "不许借助任何的工具和外力",
    "直到完整完成这件事再停止",
    "整个过程不能被任何人察觉",
    "做动作时一定要自然不刻意",
    "全程保持沉默一句话也不说",
    "完成后要向我汇报结果",
    "必须独自一人完成不能找人帮忙",
    "做这件事时要放着哈基米音乐",
    "完成后把结果发布到你的社交平台"
}

-- UI 构建
lv.obj_set_style_bg_color(page, 0x000000, 0)
ui.hide_topbar()

local logo = lv.img_create(page)
lv.img_set_src(logo, "assets/prescript_logo.png") -- 假设路径
lv.obj_align(logo, lv.ALIGN_TOP_MID, 0, 5)

local lbl_content = lv.label_create(page)
lv.obj_set_width(lbl_content, 280)
lv.obj_set_style_text_font(lbl_content, unifont_24, 0)
lv.label_set_recolor(lbl_content, true)
lv.obj_set_style_text_align(lbl_content, lv.TEXT_ALIGN_CENTER, 0)
lv.obj_align(lbl_content, lv.ALIGN_CENTER, 0, 60)
lv.label_set_text(lbl_content, idle_text)
lv.obj_set_style_text_color(lbl_content, 0x15dcff, 0)
lv.obj_add_flag(lbl_content, lvgl.FLAG_CLICKABLE)

local function split_utf8_chars(text)
    local chars = {}
    local i = 1
    local len = #text
    while i <= len do
        local b = string.byte(text, i)
        local step = 1
        if b and b >= 0xF0 then
            step = 4
        elseif b and b >= 0xE0 then
            step = 3
        elseif b and b >= 0xC0 then
            step = 2
        end
        chars[#chars + 1] = string.sub(text, i, i + step - 1)
        i = i + step
    end
    return chars
end

local function random_mask_char()
    local idx = esp.random(#mask_chars)
    return string.sub(mask_chars, idx, idx)
end

local function stop_decrypt_anim()
    if active_timer then
        lv.timer_del(active_timer)
        active_timer = nil
    end
end

local function start_decrypt_anim(target_text)
    stop_decrypt_anim()

    local chars = split_utf8_chars(target_text)
    local total = #chars
    local revealed = 0
    local tick = 0

    local function timer_cb(timer)
        if not lv.obj_is_valid(lbl_content) then
            if active_timer == timer then
                active_timer = nil
            end
            lv.timer_del(timer)
            return
        end

        tick = tick + 1
        if revealed < total and (tick % reveal_every == 0) then
            revealed = revealed + 1
        end

        local parts = {}
        for i = 1, total do
            local ch = chars[i]
            if i <= revealed then
                parts[#parts + 1] = ch
            elseif ch == " " then
                parts[#parts + 1] = " "
            else
                parts[#parts + 1] = random_mask_char()
            end
        end

        lv.label_set_text(lbl_content, table.concat(parts))

        if revealed >= total then
            lv.label_set_text(lbl_content, target_text)
            if active_timer == timer then
                active_timer = nil
            end
            lv.timer_del(timer)
        end
    end

    active_timer = lv.timer_create(timer_cb, speed_ms, nil)
end

local function pick_random_text()
    if esp.random(100) < 20 then
        return egg[esp.random(#egg)]
    end

    local text = scn[esp.random(#scn)] .. act[esp.random(#act)]
    if esp.random(100) < 35 then
        text = text .. "，" .. sup[esp.random(#sup)]
    end
    return text
end

local function reset_page_state()
    stop_decrypt_anim()
    if lv.obj_is_valid(lbl_content) then
        lv.label_set_text(lbl_content, idle_text)
    end
end

-- 点击事件
lv.obj_add_event_cb(lbl_content, function(e)
    local code = lv.event_get_code(e)
    if code == lv.EVENT_CLICKED then
        start_decrypt_anim(pick_random_text())
    end
end, lv.EVENT_CLICKED)

lv.obj_add_event_cb(page, function(e)
    local code = lv.event_get_code(e)
    if code == lv.EVENT_SCREEN_UNLOAD_START then
        reset_page_state()
    end
end, lv.EVENT_SCREEN_UNLOAD_START)

-- 主循环
sys.timerLoopStart(function()
    lvgl.poll_events(10)
end, 16)

sys.run()
