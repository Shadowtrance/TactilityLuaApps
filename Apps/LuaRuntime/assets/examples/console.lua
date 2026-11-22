-- A small Lua console: type an expression or a few statements, press Run (or
-- the keyboard's ready key) and see the result. `print` is captured into the
-- output panel, so it also works for multi-line scripts.
--
-- This runs in the same Lua state as the rest of the app, so it can call the
-- `tactility` and `lvgl` APIs directly. As with any script, a `while true do
-- end` will hang the app, so keep experiments short.

local root = lvgl.root()
root:set{ flex_flow = lvgl.FLEX_FLOW.COLUMN }

local gap = lvgl.theme_lerp(4, 10)

local header = lvgl.Object(root, {
    w = lvgl.PCT(100), h = lvgl.SIZE_CONTENT,
    flex_flow = lvgl.FLEX_FLOW.ROW,
    flex_align = { main = lvgl.FLEX_ALIGN.START, cross = lvgl.FLEX_ALIGN.CENTER, track = lvgl.FLEX_ALIGN.CENTER },
    pad_all = 0, pad_column = gap,
    bg_opa = lvgl.OPA.TRANSP, border_width = 0,
})
lvgl.Label(header, { text = "Lua Console", text_font = "large", flex_grow = 1 })
local run_button = lvgl.TextButton(header)
lvgl.Label(run_button, { text = "Run", align = lvgl.ALIGN.CENTER })
local clear_button = lvgl.TextButton(header)
lvgl.Label(clear_button, { text = "Clear", align = lvgl.ALIGN.CENTER })

local input = lvgl.TextArea(root, {
    w = lvgl.PCT(100),
    h = lvgl.theme_lerp(64, 96),
    text = "",
    one_line = false,
    placeholder = "print('hello from Lua')",
    text_font = "small",
})

local output_card = lvgl.Card(root, {
    w = lvgl.PCT(100), flex_grow = 1,
    pad_all = lvgl.theme_lerp(8, 14),
})
local output = lvgl.Label(output_card, {
    text = "Ready.",
    w = lvgl.PCT(100),
    text_font = "small",
    long_mode = lvgl.LONG.WRAP,
})

-- With a real keyboard attached, keep the screen free for input and output and
-- route keys into the text area. Otherwise show the on-screen keyboard.
local hardware_keyboard = lvgl.hardware_keyboard_available()
local key_group
local keyboard
if hardware_keyboard then
    key_group = lvgl.Group()
    key_group:add(input)
    key_group:focus(input)
    key_group:attach_input()
else
    keyboard = lvgl.Keyboard(root, {
        w = lvgl.PCT(100),
        h = lvgl.theme_lerp(160, 210),
        textarea = input,
        popovers = true,
    })
end

local MAX_LINES = 40
local lines = {}

local function refresh()
    if #lines == 0 then
        output:set{ text = "Ready." }
    else
        output:set{ text = table.concat(lines, "\n") }
    end
end

local function append(text)
    lines[#lines + 1] = text
    while #lines > MAX_LINES do
        table.remove(lines, 1)
    end
    refresh()
end

-- Loaded snippets call this instead of writing to the serial console.
print = function(...)
    local parts = {}
    for index = 1, select('#', ...) do
        parts[#parts + 1] = tostring(select(index, ...))
    end
    append(table.concat(parts, "\t"))
end

local function run()
    -- Clicking Run can move focus off the text area; keep hardware typing
    -- pointed at the prompt for the next snippet.
    if key_group ~= nil then
        key_group:focus(input)
    end

    local code = input:text()
    if code == nil or code == "" then
        return
    end
    append("> " .. code:gsub("\n", " "))

    local chunk, compile_error = load(code, "=console")
    if chunk == nil then
        append("error: " .. tostring(compile_error))
        return
    end

    local results = table.pack(pcall(chunk))
    if not results[1] then
        append("error: " .. tostring(results[2]))
        return
    end
    local values = {}
    for index = 2, results.n do
        values[#values + 1] = tostring(results[index])
    end
    if #values > 0 then
        append(table.concat(values, "\t"))
    end
end

run_button:on(lvgl.EVENT.CLICKED, run)
input:on(lvgl.EVENT.READY, run)
if keyboard ~= nil then
    keyboard:on(lvgl.EVENT.READY, run)
end
clear_button:on(lvgl.EVENT.CLICKED, function()
    lines = {}
    refresh()
    input:set{ text = "" }
end)

result = "lua console"
