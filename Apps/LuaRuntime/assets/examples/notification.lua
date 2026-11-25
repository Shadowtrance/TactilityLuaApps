-- Post and manage notifications through the firmware notification module.
--
-- The three send buttons use the theme's info/warning/error accents, and the
-- history card lists what the module currently holds (newest first). The
-- subscription at the bottom shows the live event stream, so the history and
-- event line update as soon as the module changes.

local SEVERITY = tactility.notification and tactility.notification.SEVERITY
local status_label
local list_label
local event_label
local subscription
local sent = 0

local function channel(value, shift)
    return math.floor(value / 2 ^ shift) % 256
end

local function text_on(color)
    local luminance = 0.299 * channel(color, 16) + 0.587 * channel(color, 8) + 0.114 * channel(color, 0)
    return luminance > 140 and 0x000000 or 0xffffff
end

local function refresh()
    local items = tactility.notification.list() or {}
    status_label:set{ text = string.format('%d notification(s) in history', #items) }

    local lines = {}
    for i = 1, math.min(#items, 6) do
        local item = items[i]
        local title = item.title ~= '' and (item.title .. ': ') or ''
        lines[#lines + 1] = string.format('[%s] %s%s', string.upper(item.severity), title, item.message)
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

local function action_button(parent, text, color, callback)
    local button = lvgl.Button(parent, {
        h = lvgl.theme_lerp(36, 48),
        flex_grow = 1,
        bg_color = color,
        bg_opa = lvgl.OPA.COVER,
        border_width = 0,
        radius = lvgl.theme_lerp(8, 16),
    })
    lvgl.Label(button, {
        text = text,
        align = lvgl.ALIGN.CENTER,
        text_color = text_on(color),
    })
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

    local gap = lvgl.theme_lerp(4, 10)

    local header = lvgl.Object(screen, {
        w = lvgl.PCT(100), h = lvgl.SIZE_CONTENT,
        flex_flow = lvgl.FLEX_FLOW.ROW,
        flex_align = { main = lvgl.FLEX_ALIGN.START, cross = lvgl.FLEX_ALIGN.CENTER, track = lvgl.FLEX_ALIGN.CENTER },
        pad_all = 0, pad_column = gap,
        bg_opa = lvgl.OPA.TRANSP, border_width = 0,
    })
    lvgl.Label(header, { text = 'Notifications', text_font = 'large', flex_grow = 1 })

    local refresh_button = lvgl.TextButton(header)
    lvgl.Label(refresh_button, { text = 'Refresh', align = lvgl.ALIGN.CENTER })
    refresh_button:on(lvgl.EVENT.CLICKED, refresh)

    local clear_button = lvgl.TextButton(header)
    lvgl.Label(clear_button, { text = 'Clear', align = lvgl.ALIGN.CENTER })
    clear_button:on(lvgl.EVENT.CLICKED, function()
        tactility.notification.dismiss_all()
        refresh()
    end)

    local actions = lvgl.Object(screen, {
        w = lvgl.PCT(100), h = lvgl.SIZE_CONTENT,
        flex_flow = lvgl.FLEX_FLOW.ROW,
        flex_align = { main = lvgl.FLEX_ALIGN.START, cross = lvgl.FLEX_ALIGN.CENTER, track = lvgl.FLEX_ALIGN.CENTER },
        pad_all = 0, pad_column = gap,
        bg_opa = lvgl.OPA.TRANSP, border_width = 0,
    })
    action_button(actions, 'Info', lvgl.theme_color_primary(), function()
        send(SEVERITY.INFO, 'Lua info')
    end)
    action_button(actions, 'Warning', lvgl.theme_color_secondary(), function()
        send(SEVERITY.WARNING, 'Lua warning')
    end)
    action_button(actions, 'Error', lvgl.theme_color_error(), function()
        send(SEVERITY.ERROR, 'Lua error')
    end)

    local card = lvgl.Card(screen, {
        w = lvgl.PCT(100),
        flex_grow = 1,
        flex_flow = lvgl.FLEX_FLOW.COLUMN,
        pad_all = lvgl.theme_lerp(10, 20),
        pad_row = gap,
    })

    status_label = lvgl.Label(card, { text = '...' })
    list_label = lvgl.Label(card, {
        text = '',
        w = lvgl.PCT(100),
        long_mode = lvgl.LONG.WRAP,
        text_font = 'small',
    })
    event_label = lvgl.Label(card, {
        text = 'Listening for events...',
        w = lvgl.PCT(100),
        text_font = 'small',
        long_mode = lvgl.LONG.WRAP,
    })

    subscription = tactility.notification.subscribe(function(event)
        if event_label then
            event_label:set{
                text = string.format('event: %s #%d (order %d)', event.type, event.id, event.order),
            }
        end
        refresh()
    end)
    if subscription == nil then
        event_label:set{ text = 'The event stream is not available in this runner.' }
    end

    refresh()
    result = 'notification example'
end

function on_destroy()
    if subscription then
        subscription:cancel()
        subscription = nil
    end
end
