-- Shared icon set check, including the icons the newer firmware added.
--
-- The glyphs come from the firmware's Material Symbols subset; the runner's
-- lvgl.ICON table is compiled from the SDK's shared.h, so this catches a stale
-- runner relative to the installed firmware font.

local expected = {
    "ADD", "CLOSE", "MEMORY", "WIFI",
    -- Added in firmware commit a0439804 (System Info task states).
    "PAUSE", "RADIO_BUTTON_UNCHECKED",
}

assert(lvgl.ICON, "lvgl.ICON is missing")

local missing = {}
for _, name in ipairs(expected) do
    local glyph = lvgl.ICON[name]
    if type(glyph) ~= "string" or #glyph == 0 then
        missing[#missing + 1] = name
    end
end

assert(#missing == 0, "missing icons: " .. table.concat(missing, ", "))

result = string.format("icons PASS: %d checks", #expected)
