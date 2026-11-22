-- Post and manage notifications through the firmware notification module.

local SEVERITY = tactility.notification and tactility.notification.SEVERITY
local status_label
local list_label
local sent = 0

local function refresh()
    local items = tactility.notification.list() or {}
    status_label:set{ text = string.format('%d notification(s) in history', #items) }

    local lines = {}
    for i = 1, math.min(#items, 4) do
        local item = items[i]
        local title = item.title ~= '' and (item.title .. ': ') or ''
        lines[#lines + 1] = string.format('[%s] %s%s', item.severity, title, item.message)
    end
    list_label:set{ text = #lines > 0 and table.concat(lines, '\n') or 'Nothing posted yet.' }
end

local function send(severity, title)
    sent = sent + 1
    local message = string.format('Lua notification #%d', sent)
    local id, reason = tactility.notification.send{
        message = message,
        title = title,
        severity = severity,
        source = 'lua.notification.demo',
    }
    if id == nil then
        status_label:set{ text = 'Send failed: ' .. tostring(reason) }
        return
    end
    refresh()
end

local function button(parent, text, callback)
    local button = lvgl.Button(parent, { w = lvgl.PCT(100), h = 36 })
    lvgl.Label(button, { text = text, align = lvgl.ALIGN.CENTER })
    button:on(lvgl.EVENT.CLICKED, callback)
    return button
end

function on_show()
    local screen = lvgl.root()
    screen:set{ flex_flow = lvgl.FLEX_FLOW.COLUMN }

    if not SEVERITY then
        lvgl.Label(screen, { text = 'The notification module is not available.' })
        return
    end

    local card = lvgl.Card(screen, {
        w = lvgl.PCT(100),
        h = lvgl.SIZE_CONTENT,
        flex_flow = lvgl.FLEX_FLOW.COLUMN,
    })

    status_label = lvgl.Label(card, { text = '...' })
    list_label = lvgl.Label(card, {
        text = '',
        w = lvgl.PCT(100),
        long_mode = lvgl.LONG.WRAP,
        text_font = 'small',
    })

    local row = lvgl.Object(card, {
        w = lvgl.PCT(100),
        h = lvgl.SIZE_CONTENT,
        flex_flow = lvgl.FLEX_FLOW.COLUMN,
    })

    button(row, 'Info', function() send(SEVERITY.INFO, 'Lua info') end)
    button(row, 'Warning', function() send(SEVERITY.WARNING, 'Lua warning') end)
    button(row, 'Error', function() send(SEVERITY.ERROR, 'Lua error') end)

    local actions = lvgl.Object(card, {
        w = lvgl.PCT(100),
        h = lvgl.SIZE_CONTENT,
        flex_flow = lvgl.FLEX_FLOW.COLUMN,
    })

    button(actions, 'Refresh', refresh)
    button(actions, 'Dismiss all', function()
        tactility.notification.dismiss_all()
        refresh()
    end)

    refresh()
    result = 'notification example'
end
