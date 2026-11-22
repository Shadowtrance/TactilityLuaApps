-- A Tactility app written entirely in Lua. No C, no ELF, no ESP-IDF to build it.
--
-- The chunk runs once when the app is created, and receives its install directory as the
-- first vararg, so assets resolve the same from SD and from internal flash.
local app_dir = ...

-- Lifecycle functions are globals, all optional:
--   on_create()  - after the script loads
--   on_show()    - the UI is built here; lvgl.root() is this app's screen
--   on_hide()    - the screen is being torn down
--   on_destroy() - last call before the runtime closes

local taps = 0
local tap_label
local uptime_label

function on_create()
    tactility.log.info('luahello', 'created, installed at ' .. app_dir)
end

function on_show()
    local screen = lvgl.root()
    screen:set{ flex_flow = lvgl.FLEX_FLOW.COLUMN, pad_all = 0, pad_row = 0 }

    -- lvgl.toolbar.create(screen, 'Lua Hello')

    local page = lvgl.Object(screen, {
        w = lvgl.PCT(100), flex_grow = 1,
        bg_color = 0x101018, border_width = 0, radius = 0,
        pad_all = 8, pad_row = 6, flex_flow = lvgl.FLEX_FLOW.COLUMN,
    })

    local card = lvgl.Style{ bg_color = 0x1c1c28, border_width = 0, radius = 6, pad_all = 8 }
    local caption = lvgl.Style{ text_color = 0x8899aa, text_font = 'small' }

    local intro = lvgl.Object(page, {
        w = lvgl.PCT(100), h = lvgl.SIZE_CONTENT,
        flex_flow = lvgl.FLEX_FLOW.COLUMN, pad_row = 2,
    })
    intro:add_style(card)
    lvgl.Label(intro, { text = 'This app is a single .lua file.', text_color = 0xffffff })
    lvgl.Label(intro, { text = 'No C, no ELF, no per-platform build.' }):add_style(caption)

    -- Button and a counter, to show events working
    local tap_row = lvgl.Object(page, {
        w = lvgl.PCT(100), h = lvgl.SIZE_CONTENT, flex_flow = lvgl.FLEX_FLOW.ROW,
    })
    tap_row:add_style(card)
    tap_label = lvgl.Label(tap_row, { text = 'Taps: 0', w = 200 })
    tap_label:add_style(caption)

    local button = lvgl.Button(tap_row, { w = 110, h = 32 })
    lvgl.Label(button, { text = 'Tap me', align = lvgl.ALIGN.CENTER })
    button:on(lvgl.EVENT.CLICKED, function()
        taps = taps + 1
        tap_label:set{ text = 'Taps: ' .. taps }
    end)

    -- A timer, to show the runtime staying alive between callbacks
    local uptime_row = lvgl.Object(page, {
        w = lvgl.PCT(100), h = lvgl.SIZE_CONTENT, flex_flow = lvgl.FLEX_FLOW.ROW,
    })
    uptime_row:add_style(card)
    lvgl.Label(uptime_row, { text = 'Uptime', w = 200 }):add_style(caption)
    uptime_label = lvgl.Label(uptime_row, { text = '0 s', text_color = 0xffffff })

    local function tick()
        uptime_label:set{ text = math.floor(tactility.time.uptime() / 1000) .. ' s' }
    end
    tick()
    lvgl.Timer{ period = 1000, callback = tick }

    tactility.log.info('luahello', 'shown')
end

function on_hide()
    -- Widgets and timers are cleaned up by the runtime, so there is nothing to undo here.
    tactility.log.info('luahello', 'hidden')
end

function on_destroy()
    tactility.log.info('luahello', 'destroyed after ' .. taps .. ' taps')
end
