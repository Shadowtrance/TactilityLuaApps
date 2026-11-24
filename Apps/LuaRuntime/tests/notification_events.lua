-- Notification event stream: subscribe, receive posted/dismissed/cleared.

local checks = 0
local events = {}
local subscription
local check_timer
local first_id
local second_id
local third_id

local function check(condition, message)
    checks = checks + 1
    if not condition then
        error("notification_events: " .. (message or ("check " .. checks .. " failed")), 2)
    end
end

local function run_checks()
    check(#events == 5, "expected 5 events, received " .. #events)

    check(events[1].type == "posted", "first event is posted")
    check(events[1].id == first_id, "first event id")
    check(events[2].type == "dismissed", "second event is dismissed")
    check(events[2].id == first_id, "dismissed event id")
    check(events[3].type == "posted" and events[3].id == second_id, "second posted")
    check(events[4].type == "posted" and events[4].id == third_id, "third posted")
    check(events[5].type == "cleared", "last event is cleared")
    check(events[5].id == 0, "cleared event has no id")

    check(type(events[1].timestamp) == "number" and events[1].timestamp > 0, "timestamp")
    for index = 2, #events do
        check(events[index].order > events[index - 1].order, "event order increases")
    end

    subscription:cancel()
    check(not subscription:is_active(), "cancel stops the subscription")

    result = string.format(
        "notification_events PASS: %d events, %d checks",
        #events,
        checks
    )
end

function on_show()
    check(tactility.notification.EVENT.POSTED == "posted", "event constants")

    local ok, error_message = pcall(tactility.notification.subscribe, "not a function")
    check(not ok and tostring(error_message):find("function") ~= nil, "callback is required")

    subscription = tactility.notification.subscribe(function(event)
        events[#events + 1] = event
    end)
    check(subscription ~= nil and subscription:is_active(), "subscribe")

    first_id = tactility.notification.send{
        message = "Event test 1",
        source = "lua.notification.events",
    }
    check(type(first_id) == "number", "first send")
    check(tactility.notification.dismiss(first_id), "dismiss")

    second_id = tactility.notification.send{
        message = "Event test 2",
        source = "lua.notification.events",
    }
    third_id = tactility.notification.send{
        message = "Event test 3",
        source = "lua.notification.events",
    }
    check(type(second_id) == "number" and type(third_id) == "number", "second and third send")

    tactility.notification.dismiss_all()

    -- The events are delivered by a 200 ms poll timer, so wait well past one tick.
    check_timer = lvgl.Timer{
        period = 800,
        callback = function()
            check_timer:delete()

            local ok_run, error_run = pcall(run_checks)
            if not ok_run then
                result = tostring(error_run)
            end
        end,
    }
end
