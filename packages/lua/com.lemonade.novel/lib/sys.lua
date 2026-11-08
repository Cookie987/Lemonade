-- 本项目只需要 sys 的定时器与主循环能力，这里实现一个最小子集。
-- 与固件自带的 sys.lua 保持相同的定时器调用方式：
--   sys.timerStart(fn, ms)
--   sys.timerLoopStart(fn, ms)
--   sys.timerStop(id)
--   sys.run()
-- 本应用不使用协程任务，因此没有实现 sys.taskInit / sys.wait。

local sys = {}

local rtos = _G.rtos
local MAX_TIMER_ID = 0x1FFFFF
local timers = {}
local next_timer_id = 0

local function alloc_timer_id()
    for _ = 1, MAX_TIMER_ID do
        next_timer_id = next_timer_id + 1
        if next_timer_id > MAX_TIMER_ID then
            next_timer_id = 1
        end
        if timers[next_timer_id] == nil then
            return next_timer_id
        end
    end
    return nil
end

local function start_timer(callback, ms, repeat_mode)
    if type(callback) ~= "function" then
        return nil
    end
    ms = tonumber(ms) or 0
    if ms < 1 then
        ms = 1
    end

    local timer_id = alloc_timer_id()
    if not timer_id then
        return nil
    end

    timers[timer_id] = {
        callback = callback,
        repeat_mode = repeat_mode,
    }
    if rtos.timer_start(timer_id, ms, repeat_mode) ~= 1 then
        timers[timer_id] = nil
        return nil
    end
    return timer_id
end

function sys.timerStart(callback, ms)
    return start_timer(callback, ms, 0)
end

function sys.timerLoopStart(callback, ms)
    return start_timer(callback, ms, -1)
end

function sys.timerStop(timer_id)
    if type(timer_id) ~= "number" then
        return
    end
    timers[timer_id] = nil
    rtos.timer_stop(timer_id)
end

function sys.timerIsActive(timer_id)
    return timers[timer_id] ~= nil
end

function sys.safeRun()
    local message, timer_id, repeat_mode = rtos.receive(rtos.INF_TIMEOUT)
    if message == rtos.MSG_TIMER and timers[timer_id] then
        local timer = timers[timer_id]
        if repeat_mode == 0 then
            timers[timer_id] = nil
        end
        timer.callback()
    end
end

function sys.run()
    while true do
        sys.safeRun()
    end
end

return sys
