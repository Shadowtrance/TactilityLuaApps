-- The launcher-style paged tile grid: items, styles, paging and lookup.

local items = {}
local names = { "One", "Two", "Three", "Four", "Five", "Six", "Seven", "Eight", "Nine", "Ten", "Eleven", "Twelve" }
for index, name in ipairs(names) do
    items[index] = {
        id = "item-" .. index,
        name = name,
        icon = lvgl.ICON.APPS,
        highlighted = index == 1,
    }
end

local clicked = nil
local grid = lvgl.TileGrid(lvgl.root(), {
    w = lvgl.PCT(100),
    h = lvgl.PCT(100),
    items = items,
    style = "TILE",
    icon_color = "DEFAULT",
    swipe = true,
    on_clicked = function(item)
        clicked = item
    end,
    on_page_changed = function(page)
        clicked = page
    end,
})

assert(grid ~= nil, "tile grid")
assert(grid:page() == 1, "first page")
assert(grid:find_tile("item-1") ~= nil, "find the first tile")
assert(grid:find_tile("missing") == nil, "missing tile")
assert(grid:tiles() ~= nil, "tiles container")

grid:set_style("CARD")
grid:set_icon_color("PRIMARY")
grid:next_page()
grid:previous_page()
grid:set_items({
    { id = "one", name = "One", icon = lvgl.ICON.FOLDER },
    { id = "two", name = "Two", icon = lvgl.ICON.DESCRIPTION },
})

-- set_items() requests a rebuild; next_page() rebuilds right away.
grid:next_page()
grid:previous_page()

assert(grid:find_tile("one") ~= nil, "set_items")
assert(grid:find_tile("item-1") == nil, "old items are gone")

result = string.format("tile grid PASS: %d items, page %d, %s", #names, grid:page(), tostring(clicked))
