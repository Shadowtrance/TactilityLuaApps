-- A UI drawn entirely by Lua through the luavgl bindings.
--
-- Everything here uses the theme's own widget classes and colours. The cards,
-- button and text all pick up the device's appearance settings; there are no
-- hardcoded surface colours.

local screen = lvgl.root()
screen:set{ flex_flow = lvgl.FLEX_FLOW.COLUMN }

local intro = lvgl.Card(screen, {
    w = lvgl.PCT(100),
    h = lvgl.SIZE_CONTENT,
    flex_flow = lvgl.FLEX_FLOW.COLUMN,
})
lvgl.Label(intro, { text = 'This UI is drawn by Lua.' })
lvgl.Label(intro, { text = 'No C layout for the content.', text_font = 'small' })

local row = lvgl.Card(screen, {
    w = lvgl.PCT(100),
    h = lvgl.SIZE_CONTENT,
    flex_flow = lvgl.FLEX_FLOW.ROW,
    flex_align = {
        main = lvgl.FLEX_ALIGN.START,
        cross = lvgl.FLEX_ALIGN.CENTER,
        track = lvgl.FLEX_ALIGN.CENTER,
    },
})

local count = 0
local label = lvgl.Label(row, { text = 'Taps: 0', flex_grow = 1 })

local button = lvgl.Button(row, { w = 120, h = 36 })
lvgl.Label(button, { text = 'Tap me', align = lvgl.ALIGN.CENTER })
button:on(lvgl.EVENT.CLICKED, function()
    count = count + 1
    label:set{ text = 'Taps: ' .. count }
end)

result = 'ui example'
