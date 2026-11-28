// SPDX-License-Identifier: Apache-2.0
#include <luavgl/object_private.h>

#include <lvgl/widgets/tile_grid.h>

#include <tactility/log.h>

#include <cstring>
#include <initializer_list>
#include <new>
#include <string>
#include <vector>

extern "C" {
#include <lauxlib.h>
}

/**
 * `lvgl.TileGrid(parent?, { items = {...}, style = "TILE", ... })`
 *
 * The firmware's paged launcher grid. Items are copied into C data before the grid
 * is created, because the grid calls its collect callback during a layout pass.
 *
 * Click and page callbacks are queued with lv_async_call(), not called from the
 * widget's event callback: a Lua handler may delete the grid, and the firmware must
 * not continue using it after that. The queue is cancelled when the runtime closes.
 */

namespace {

constexpr auto* TAG = "luavgl.tile_grid";

char TILE_GRIDS_KEY = 0;

struct TileGridItem {
    std::string id;
    std::string name;
    std::string icon;
    bool highlighted = false;
    bool checked = false;
};

struct TileGridRequest {
    lua_State* state = nullptr;
    lv_obj_t* grid = nullptr;
    int clicked_ref = LUA_NOREF;
    int long_pressed_ref = LUA_NOREF;
    int page_changed_ref = LUA_NOREF;

    bool state_alive = true;
    bool cancelled = false;

    std::vector<TileGridItem> items;
    /** Items from the last set_items() call, kept until the rebuild deleted the old tiles. */
    std::vector<std::vector<TileGridItem>> retired;

    LvglTileGridPaging paging = {};
    LvglTileGridIndicator indicator = LVGL_TILE_GRID_INDICATOR_BELOW;

    TileGridRequest* next = nullptr;
};

TileGridRequest* tile_grids = nullptr;

enum class PendingKind {
    Clicked,
    LongPressed,
    PageChanged,
};

struct PendingTileEvent {
    lua_State* state = nullptr;
    int ref = LUA_NOREF;
    lv_obj_t* grid = nullptr;
    PendingKind kind = PendingKind::Clicked;
    std::string id;
    std::string name;
    std::string icon;
    bool highlighted = false;
    bool checked = false;
    uint32_t page = 0;
    bool cancelled = false;
    PendingTileEvent* next = nullptr;
};

PendingTileEvent* pending_events = nullptr;

void push_tile_grids(lua_State* state) {
    lua_rawgetp(state, LUA_REGISTRYINDEX, &TILE_GRIDS_KEY);
    if (lua_isnil(state, -1)) {
        lua_pop(state, 1);
        lua_newtable(state);
        lua_pushvalue(state, -1);
        lua_rawsetp(state, LUA_REGISTRYINDEX, &TILE_GRIDS_KEY);
    }
}

TileGridRequest* find_tile_grid(lua_State* state, lv_obj_t* grid) {
    push_tile_grids(state);
    lua_pushlightuserdata(state, grid);
    const int type = lua_rawget(state, -2);
    TileGridRequest* request = nullptr;
    if (type == LUA_TLIGHTUSERDATA) {
        request = static_cast<TileGridRequest*>(lua_touserdata(state, -1));
    }
    lua_pop(state, 2);
    return request;
}

void add_tile_grid(lua_State* state, TileGridRequest* request) {
    push_tile_grids(state);
    lua_pushlightuserdata(state, request->grid);
    lua_pushlightuserdata(state, request);
    lua_rawset(state, -3);
    lua_pop(state, 1);
}

void remove_tile_grid(lua_State* state, lv_obj_t* grid) {
    push_tile_grids(state);
    lua_pushlightuserdata(state, grid);
    lua_pushnil(state);
    lua_rawset(state, -3);
    lua_pop(state, 1);
}

void unlink_request(TileGridRequest* request) {
    for (TileGridRequest** link = &tile_grids; *link != nullptr; link = &(*link)->next) {
        if (*link == request) {
            *link = request->next;
            return;
        }
    }
}

void unref(int& ref, lua_State* state) {
    if (ref != LUA_NOREF) {
        luaL_unref(state, LUA_REGISTRYINDEX, ref);
        ref = LUA_NOREF;
    }
}

/** Frees a request; the state is only touched when it is still alive. */
void free_request(TileGridRequest* request) {
    if (request->state_alive && request->state != nullptr) {
        unref(request->clicked_ref, request->state);
        unref(request->long_pressed_ref, request->state);
        unref(request->page_changed_ref, request->state);
        remove_tile_grid(request->state, request->grid);
    }
    delete request;
}

void on_grid_deleted(lv_event_t* event) {
    auto* request = static_cast<TileGridRequest*>(lv_event_get_user_data(event));
    unlink_request(request);
    free_request(request);
}

LvglTileGridIndicator get_indicator(lv_obj_t* grid) {
    auto* request = static_cast<TileGridRequest*>(lv_obj_get_user_data(grid));
    return request != nullptr ? request->indicator : LVGL_TILE_GRID_INDICATOR_BELOW;
}

void fill_item(lv_obj_t* grid, void* user_data) {
    auto* request = static_cast<TileGridRequest*>(user_data);
    if (request->cancelled) {
        return;
    }

    // The grid has already deleted the old tiles before collect runs, so their icon
    // strings can go now.
    request->retired.clear();

    for (const auto& item : request->items) {
        LvglTileGridItem entry = {};
        entry.id = item.id.c_str();
        entry.name = item.name.c_str();
        entry.icon = item.icon.empty() ? nullptr : item.icon.c_str();
        entry.highlighted = item.highlighted;
        entry.checked = item.checked;
        lvgl_tile_grid_add_item(grid, &entry);
    }
}

void unlink_pending(PendingTileEvent* event) {
    for (PendingTileEvent** link = &pending_events; *link != nullptr; link = &(*link)->next) {
        if (*link == event) {
            *link = event->next;
            return;
        }
    }
}

/** Runs a queued click/page callback after the widget event has fully unwound. */
void run_pending_event(void* context) {
    auto* event = static_cast<PendingTileEvent*>(context);
    unlink_pending(event);

    const bool cancelled = event->cancelled;
    lua_State* state = event->state;
    const int ref = event->ref;
    if (cancelled || state == nullptr || ref == LUA_NOREF) {
        delete event;
        return;
    }

    lua_rawgeti(state, LUA_REGISTRYINDEX, ref);
    if (lua_type(state, -1) != LUA_TFUNCTION) {
        lua_pop(state, 1);
        luaL_unref(state, LUA_REGISTRYINDEX, ref);
        delete event;
        return;
    }

    if (event->kind == PendingKind::PageChanged) {
        lua_pushinteger(state, static_cast<lua_Integer>(event->page) + 1);
    } else {
        lua_createtable(state, 0, 6);
        lua_pushlstring(state, event->id.data(), event->id.size());
        lua_setfield(state, -2, "id");
        lua_pushlstring(state, event->name.data(), event->name.size());
        lua_setfield(state, -2, "name");
        if (!event->icon.empty()) {
            lua_pushlstring(state, event->icon.data(), event->icon.size());
            lua_setfield(state, -2, "icon");
        }
        lua_pushboolean(state, event->highlighted);
        lua_setfield(state, -2, "highlighted");
        lua_pushboolean(state, event->checked);
        lua_setfield(state, -2, "checked");
    }

    if (lua_pcall(state, 1, 0, 0) != LUA_OK) {
        LOG_E(TAG, "tile grid callback: %s", lua_tostring(state, -1));
        lua_pop(state, 1);
    }
    luaL_unref(state, LUA_REGISTRYINDEX, ref);
    delete event;
}

int queue_event(
    TileGridRequest* request,
    const LvglTileGridItem* item,
    int callback_ref,
    PendingKind kind,
    uint32_t page = 0
) {
    if (request->cancelled || !request->state_alive || callback_ref == LUA_NOREF) {
        return 0;
    }

    lua_State* state = request->state;
    lua_rawgeti(state, LUA_REGISTRYINDEX, callback_ref);
    if (lua_type(state, -1) != LUA_TFUNCTION) {
        lua_pop(state, 1);
        return 0;
    }
    const int event_ref = luaL_ref(state, LUA_REGISTRYINDEX);

    auto* event = new (std::nothrow) PendingTileEvent();
    if (event == nullptr) {
        luaL_unref(state, LUA_REGISTRYINDEX, event_ref);
        return 0;
    }

    event->state = state;
    event->ref = event_ref;
    event->grid = request->grid;
    event->kind = kind;
    event->page = page;
    if (item != nullptr) {
        event->id = item->id != nullptr ? item->id : "";
        event->name = item->name != nullptr ? item->name : "";
        event->icon = item->icon != nullptr ? item->icon : "";
        event->highlighted = item->highlighted;
        event->checked = item->checked;
    }
    event->next = pending_events;
    pending_events = event;
    lv_async_call(run_pending_event, event);
    return 1;
}

void on_clicked(lv_obj_t* grid, const LvglTileGridItem* item, void* user_data) {
    auto* request = static_cast<TileGridRequest*>(user_data);
    queue_event(request, item, request->clicked_ref, PendingKind::Clicked);
}

void on_long_pressed(lv_obj_t* grid, const LvglTileGridItem* item, void* user_data) {
    auto* request = static_cast<TileGridRequest*>(user_data);
    queue_event(request, item, request->long_pressed_ref, PendingKind::LongPressed);
}

void on_page_changed(lv_obj_t* grid, void* user_data) {
    auto* request = static_cast<TileGridRequest*>(user_data);
    queue_event(
        request,
        nullptr,
        request->page_changed_ref,
        PendingKind::PageChanged,
        lvgl_tile_grid_get_page(grid)
    );
}

bool read_string(lua_State* state, int table_index, const char* key, std::string& value) {
    lua_getfield(state, table_index, key);
    const bool is_string = lua_isstring(state, -1) != 0;
    if (is_string) {
        size_t length = 0;
        const char* text = lua_tolstring(state, -1, &length);
        value.assign(text, length);
    }
    lua_pop(state, 1);
    return is_string;
}

bool read_bool(lua_State* state, int table_index, const char* key) {
    lua_getfield(state, table_index, key);
    const bool value = lua_toboolean(state, -1) != 0;
    lua_pop(state, 1);
    return value;
}

bool read_choice(
    lua_State* state,
    int table_index,
    const char* key,
    const char* const* names,
    int count,
    int& out
) {
    lua_getfield(state, table_index, key);
    if (lua_isinteger(state, -1)) {
        out = static_cast<int>(lua_tointeger(state, -1));
        lua_pop(state, 1);
        return true;
    }
    if (!lua_isstring(state, -1)) {
        lua_pop(state, 1);
        return false;
    }
    const char* value = lua_tostring(state, -1);
    for (int index = 0; index < count; index++) {
        if (std::strcmp(value, names[index]) == 0) {
            out = index;
            lua_pop(state, 1);
            return true;
        }
    }
    lua_pop(state, 1);
    return false;
}

const char* const STYLE_NAMES[] = { "TILE", "CARD" };
const char* const ICON_COLOR_NAMES[] = { "DEFAULT", "PRIMARY", "SECONDARY" };
const char* const INDICATOR_NAMES[] = { "BELOW", "LEFT", "RIGHT" };

const char* parse_items(lua_State* state, int index, std::vector<TileGridItem>& items) {
    if (lua_isnoneornil(state, index)) {
        return nullptr;
    }
    if (!lua_istable(state, index)) {
        return "items must be a table";
    }

    const lua_Integer count = luaL_len(state, index);
    items.reserve(static_cast<size_t>(count));
    for (lua_Integer item_index = 1; item_index <= count; item_index++) {
        lua_geti(state, index, item_index);
        if (!lua_istable(state, -1)) {
            lua_pop(state, 1);
            return "each item must be a table";
        }

        TileGridItem item;
        read_string(state, -1, "id", item.id);
        read_string(state, -1, "name", item.name);
        read_string(state, -1, "icon", item.icon);
        item.highlighted = read_bool(state, -1, "highlighted");
        item.checked = read_bool(state, -1, "checked");
        lua_pop(state, 1);

        if (item.name.empty()) {
            item.name = item.id;
        }
        if (item.id.empty()) {
            item.id = item.name;
        }
        if (item.id.empty()) {
            return "each item needs an id or a name";
        }
        items.push_back(std::move(item));
    }
    return nullptr;
}

int push_properties_without(lua_State* state, int table_index, std::initializer_list<const char*> excluded) {
    const int absolute_index = lua_absindex(state, table_index);

    lua_createtable(state, 0, 8);
    const int target_index = lua_gettop(state);

    lua_pushnil(state);
    while (lua_next(state, absolute_index) != 0) {
        bool skip = false;
        if (lua_type(state, -2) == LUA_TSTRING) {
            const char* key = lua_tostring(state, -2);
            for (const char* name : excluded) {
                if (std::strcmp(key, name) == 0) {
                    skip = true;
                    break;
                }
            }
        }
        if (!skip) {
            lua_pushvalue(state, -2);
            lua_pushvalue(state, -2);
            lua_settable(state, target_index);
        }
        lua_pop(state, 1);
    }

    return target_index;
}

TileGridRequest* check_request(lua_State* state) {
    lv_obj_t* object = luavgl_check_object(state, 1);
    TileGridRequest* request = find_tile_grid(state, object);
    if (request == nullptr || request->cancelled) {
        luaL_error(state, "this widget is not a tile grid");
    }
    return request;
}

} // namespace

int luavgl_widget_tile_grid(lua_State* state) {
    const auto args = luavgl_parse_constructor_args(state);

    auto* request = new (std::nothrow) TileGridRequest();
    if (request == nullptr) {
        return luaL_error(state, "out of memory");
    }
    request->state = state;

    LvglTileGridStyle style = LVGL_TILE_GRID_STYLE_TILE;
    LvglTileGridIconColor icon_color = LVGL_TILE_GRID_ICON_COLOR_DEFAULT;
    bool swipe = false;
    uint32_t page = 0;

    if (args.properties_index != 0) {
        lua_getfield(state, args.properties_index, "items");
        const char* error = parse_items(state, -1, request->items);
        lua_pop(state, 1);
        if (error != nullptr) {
            delete request;
            return luaL_error(state, "%s", error);
        }

        int choice = 0;
        if (read_choice(state, args.properties_index, "style", STYLE_NAMES, 2, choice)) {
            style = static_cast<LvglTileGridStyle>(choice);
        }
        if (read_choice(state, args.properties_index, "icon_color", ICON_COLOR_NAMES, 3, choice)) {
            icon_color = static_cast<LvglTileGridIconColor>(choice);
        }
        if (read_choice(state, args.properties_index, "indicator", INDICATOR_NAMES, 3, choice)) {
            request->indicator = static_cast<LvglTileGridIndicator>(choice);
        }
        swipe = read_bool(state, args.properties_index, "swipe");

        lua_getfield(state, args.properties_index, "page");
        if (lua_isinteger(state, -1)) {
            const lua_Integer value = lua_tointeger(state, -1);
            page = value > 0 ? static_cast<uint32_t>(value - 1) : 0;
        }
        lua_pop(state, 1);

        const struct {
            const char* name;
            int* ref;
        } callbacks[] = {
            { "on_clicked", &request->clicked_ref },
            { "on_long_pressed", &request->long_pressed_ref },
            { "on_page_changed", &request->page_changed_ref },
        };
        for (const auto& callback : callbacks) {
            lua_getfield(state, args.properties_index, callback.name);
            if (lua_isfunction(state, -1)) {
                *callback.ref = luaL_ref(state, LUA_REGISTRYINDEX);
            } else if (!lua_isnil(state, -1)) {
                lua_pop(state, 1);
                delete request;
                return luaL_error(state, "%s must be a function", callback.name);
            } else {
                lua_pop(state, 1);
            }
        }
    }

    request->paging.get_indicator = get_indicator;

    LvglTileGridConfig config = {};
    config.style = style;
    config.icon_color = icon_color;
    config.swipe_navigation = swipe;
    config.callbacks.collect = fill_item;
    config.callbacks.on_clicked = on_clicked;
    config.callbacks.on_long_pressed = on_long_pressed;
    config.callbacks.on_page_changed = on_page_changed;
    config.callbacks.user_data = request;
    config.paging = &request->paging;
    config.page = page;

    request->grid = lvgl_tile_grid_create(args.parent, &config);
    if (request->grid == nullptr) {
        delete request;
        return luaL_error(state, "could not create the tile grid");
    }

    // The paging adapter needs the request; the C grid itself does not use the outer
    // object's user data (tiles carry their own).
    lv_obj_set_user_data(request->grid, request);
    lv_obj_add_event_cb(request->grid, on_grid_deleted, LV_EVENT_DELETE, request);

    request->next = tile_grids;
    tile_grids = request;
    add_tile_grid(state, request);

    luavgl_push_object(state, request->grid);
    if (args.properties_index != 0) {
        const int properties = push_properties_without(
            state,
            args.properties_index,
            { "items", "style", "icon_color", "indicator", "swipe", "page",
              "on_clicked", "on_long_pressed", "on_page_changed" }
        );
        luavgl_apply_properties(state, request->grid, properties);
        lua_pop(state, 1);
    }

    return 1;
}

int luavgl_tile_grid_set_items(lua_State* state) {
    TileGridRequest* request = check_request(state);

    std::vector<TileGridItem> items;
    const char* error = parse_items(state, 2, items);
    if (error != nullptr) {
        return luaL_error(state, "%s", error);
    }

    request->retired.push_back(std::move(request->items));
    request->items = std::move(items);
    lvgl_tile_grid_request_repopulate(request->grid, false);

    lua_settop(state, 1);
    return 1;
}

int luavgl_tile_grid_set_style(lua_State* state) {
    TileGridRequest* request = check_request(state);

    const char* value = luaL_checkstring(state, 2);
    int choice = -1;
    for (int index = 0; index < 2; index++) {
        if (std::strcmp(value, STYLE_NAMES[index]) == 0) {
            choice = index;
            break;
        }
    }
    if (choice < 0) {
        return luaL_error(state, "style must be TILE or CARD");
    }

    lvgl_tile_grid_set_style(request->grid, static_cast<LvglTileGridStyle>(choice));
    lvgl_tile_grid_request_repopulate(request->grid, false);
    lua_settop(state, 1);
    return 1;
}

int luavgl_tile_grid_set_icon_color(lua_State* state) {
    TileGridRequest* request = check_request(state);
    const char* value = luaL_checkstring(state, 2);
    int choice = -1;
    for (int index = 0; index < 3; index++) {
        if (std::strcmp(value, ICON_COLOR_NAMES[index]) == 0) {
            choice = index;
            break;
        }
    }
    if (choice < 0) {
        return luaL_error(state, "icon color must be DEFAULT, PRIMARY or SECONDARY");
    }

    lvgl_tile_grid_set_icon_color(request->grid, static_cast<LvglTileGridIconColor>(choice));
    lvgl_tile_grid_request_repopulate(request->grid, false);
    lua_settop(state, 1);
    return 1;
}

int luavgl_tile_grid_next_page(lua_State* state) {
    lvgl_tile_grid_next_page(check_request(state)->grid);
    lua_settop(state, 1);
    return 1;
}

int luavgl_tile_grid_previous_page(lua_State* state) {
    lvgl_tile_grid_previous_page(check_request(state)->grid);
    lua_settop(state, 1);
    return 1;
}

int luavgl_tile_grid_page(lua_State* state) {
    lua_pushinteger(state, static_cast<lua_Integer>(lvgl_tile_grid_get_page(check_request(state)->grid)) + 1);
    return 1;
}

int luavgl_tile_grid_find_tile(lua_State* state) {
    TileGridRequest* request = check_request(state);
    const char* id = luaL_checkstring(state, 2);
    lv_obj_t* tile = lvgl_tile_grid_find_tile(request->grid, id);
    if (tile == nullptr) {
        lua_pushnil(state);
    } else {
        luavgl_push_object(state, tile);
    }
    return 1;
}

int luavgl_tile_grid_tiles(lua_State* state) {
    luavgl_push_object(state, lvgl_tile_grid_get_tiles(check_request(state)->grid));
    return 1;
}

void luavgl_delete_all_tile_grids(lua_State* state) {
    for (TileGridRequest* request = tile_grids; request != nullptr; request = request->next) {
        if (request->state != state) {
            continue;
        }
        request->state_alive = false;
        request->cancelled = true;
        request->state = nullptr;
        unref(request->clicked_ref, state);
        unref(request->long_pressed_ref, state);
        unref(request->page_changed_ref, state);
    }

    for (PendingTileEvent* event = pending_events; event != nullptr; event = event->next) {
        if (event->state != state) {
            continue;
        }
        event->cancelled = true;
        unref(event->ref, state);
        event->state = nullptr;
    }
}
