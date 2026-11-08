-- 验证最小 sys 的定时器分发逻辑（不启动 sys.run 的死循环）。
package.path = "./lib/?.lua;" .. package.path

local queue = {}
local started = {}
local stopped = {}

_G.rtos = {
    INF_TIMEOUT = -1,
    MSG_TIMER = 1,
    timer_start = function(id, ms, repeat_mode)
        started[id] = { ms = ms, repeat_mode = repeat_mode }
        return 1
    end,
    timer_stop = function(id)
        stopped[id] = true
        started[id] = nil
    end,
    receive = function()
        local message = table.remove(queue, 1)
        if not message then
            return -1
        end
        return message.msg, message.id, message.repeat_mode
    end,
}

local sys = dofile("lib/sys.lua")

local one_shot_count = 0
local one_shot_id = sys.timerStart(function()
    one_shot_count = one_shot_count + 1
end, 100)
assert(one_shot_id, "timerStart should return id")
assert(started[one_shot_id].repeat_mode == 0, "one shot repeat mode")
queue[#queue + 1] = { msg = rtos.MSG_TIMER, id = one_shot_id, repeat_mode = 0 }
sys.safeRun()
assert(one_shot_count == 1, "one shot callback should run once")

queue[#queue + 1] = { msg = rtos.MSG_TIMER, id = one_shot_id, repeat_mode = 0 }
sys.safeRun()
assert(one_shot_count == 1, "one shot should be removed after firing")

local loop_count = 0
local loop_id = sys.timerLoopStart(function()
    loop_count = loop_count + 1
end, 50)
assert(started[loop_id].repeat_mode == -1, "loop repeat mode")
queue[#queue + 1] = { msg = rtos.MSG_TIMER, id = loop_id, repeat_mode = -1 }
sys.safeRun()
queue[#queue + 1] = { msg = rtos.MSG_TIMER, id = loop_id, repeat_mode = -1 }
sys.safeRun()
assert(loop_count == 2, "loop callback should keep firing")

sys.timerStop(loop_id)
assert(stopped[loop_id], "timerStop should stop the rtos timer")
assert(not sys.timerIsActive(loop_id), "timer should not be active")

print("sys: timer tests passed")
