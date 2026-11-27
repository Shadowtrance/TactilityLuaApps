#include <app/event.h>
#include <app/manager.h>
#include <app/manifest.h>
#include <app/paths.h>
#include <app/scheduler.h>

#include <lvgl_window_manager/window_manager.h>

#include <lua.h>
#include <lua/bindings.h>
#include <lua/runtime.h>

#include <luavgl/bindings.h>

#include <lvgl/lvgl.h>

#include <tactility/check.h>
#include <tactility/log.h>
#include <tactility/paths.h>

#include <lvgl.h>
#include <lvgl/widgets/card.h>
#include <lvgl/widgets/toolbar.h>

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Lua script runner (hidden app).
 *
 * The companion starts this with the script's absolute path as argv[0], or a
 * packaged Lua app starts it without arguments and it runs its own
 * <install>/lua/main.lua. It owns the window, the Lua runtime and the luavgl
 * bindings for one script.
 *
 * The interpreter and kernel bindings live in the `lua_runtime` package module;
 * this app resolves `lua_*` from it at load time, exactly like an ELF app
 * binding to a firmware module. luavgl lives here because this app loads after
 * LVGL, so it can reference lv_* without the module needing a boot-order fix.
 */

struct ScriptContext {
    const char* path;
    const char* name;
    const char* script_arg;
    /** Runtime API the script package asks for; see LUA_RUNTIME_API_VERSION. */
    int required_api;
    /** The app instance whose window owns this context; used by the close action. */
    AppInstanceId app_instance_id;
    struct LuaRuntime* runtime;
    bool luavgl_open;
};

static int read_runtime_api(struct LuaRuntime* runtime) {
    lua_State* state = (lua_State*)lua_runtime_get_state(runtime);
    lua_getglobal(state, "_LUA_RUNTIME_API");
    const int api = lua_isinteger(state, -1) ? (int)lua_tointeger(state, -1) : 0;
    lua_pop(state, 1);
    return api;
}

/** Reads `lua.runtime.api=` from a package's manifest; the runner's own version is the floor. */
static int read_required_api(const char* install_dir) {
    char path[FILE_MAX_PATH_STRING_LENGTH];
    const int written = snprintf(path, sizeof(path), "%s/manifest.properties", install_dir);
    if (written <= 0 || (size_t)written >= sizeof(path)) {
        return LUA_RUNTIME_API_VERSION;
    }

    FILE* file = fopen(path, "r");
    if (file == NULL) {
        return LUA_RUNTIME_API_VERSION;
    }

    const char* prefix = "lua.runtime.api=";
    int required = LUA_RUNTIME_API_VERSION;
    char line[256];
    while (fgets(line, sizeof(line), file) != NULL) {
        if (strncmp(line, prefix, strlen(prefix)) != 0) {
            continue;
        }

        const int value = atoi(line + strlen(prefix));
        if (value > required) {
            required = value;
        }
        break;
    }

    fclose(file);
    return required;
}

static bool call_optional(struct LuaRuntime* runtime, const char* name) {
    if (!lua_runtime_has_function(runtime, name)) {
        return true;
    }

    if (lua_runtime_call(runtime, name) != ERROR_NONE) {
        LOG_E("LuaRunner", "%s failed: %s", name, lua_runtime_get_error(runtime));
        return false;
    }

    return true;
}

static void show_text(lv_obj_t* parent, const char* text, uint32_t color) {
    lv_obj_t* card = lvgl_card_create(parent);
    lv_obj_set_width(card, LV_PCT(100));
    lv_obj_set_height(card, LV_SIZE_CONTENT);

    lv_obj_t* label = lv_label_create(card);
    lv_label_set_text(label, text);
    lv_obj_set_width(label, LV_PCT(100));
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
}

/** The toolbar's close action, which the window manager's default doesn't handle. */
static void on_close(lv_event_t* event) {
    struct ScriptContext* context = (struct ScriptContext*)lv_event_get_user_data(event);
    app_event_emit_close(context->app_instance_id);
}

static void create_widgets(lv_obj_t* parent, void* user_data) {
    struct ScriptContext* context = (struct ScriptContext*)user_data;

    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(parent, 0, 0);
    lv_obj_set_style_pad_row(parent, 0, 0);

    lv_obj_t* toolbar = lvgl_toolbar_create(parent, context->name);
    lvgl_toolbar_set_nav_action(toolbar, LV_SYMBOL_CLOSE, on_close, context);

    lv_obj_t* content = lv_obj_create(parent);
    lv_obj_set_width(content, LV_PCT(100));
    lv_obj_set_flex_grow(content, 1);
    // Padding and the row gap come from the theme's container style, so the
    // content follows the user's spacing setting. Only the surface is removed,
    // leaving the window's own background.
    lv_obj_set_style_border_width(content, 0, 0);
    lv_obj_set_style_radius(content, 0, 0);
    lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);

    if (context->path == NULL) {
        show_text(content, "No script found. Run one from the Lua app, or install a packaged Lua app.", 0xf7768e);
        return;
    }

    context->runtime = lua_runtime_alloc();
    if (context->runtime == NULL) {
        show_text(content, "Failed to allocate Lua runtime", 0xf7768e);
        return;
    }

    const int runtime_api = read_runtime_api(context->runtime);
    if (runtime_api < context->required_api) {
        char message[128];
        snprintf(
            message,
            sizeof(message),
            "The Lua Runtime package is too old (API %d, this app needs %d). Update the Lua Runtime package.",
            runtime_api,
            context->required_api
        );
        show_text(content, message, 0xf7768e);
        return;
    }

    if (lua_bindings_open(context->runtime) != ERROR_NONE) {
        show_text(content, lua_runtime_get_error(context->runtime), 0xf7768e);
        return;
    }

    if (luavgl_bindings_open(context->runtime, content) != ERROR_NONE) {
        show_text(content, "Failed to open Lua LVGL bindings", 0xf7768e);
        return;
    }
    context->luavgl_open = true;

    const error_t error = lua_runtime_eval_file(context->runtime, context->path, context->script_arg);
    if (error != ERROR_NONE) {
        LOG_E(
            "LuaRunner",
            "failed to evaluate %s: %s",
            context->path,
            lua_runtime_get_error(context->runtime)
        );
        show_text(content, lua_runtime_get_error(context->runtime), 0xf7768e);
        return;
    }

    if (!call_optional(context->runtime, "on_create")) {
        show_text(content, lua_runtime_get_error(context->runtime), 0xf7768e);
        return;
    }
    if (!call_optional(context->runtime, "on_show")) {
        show_text(content, lua_runtime_get_error(context->runtime), 0xf7768e);
        return;
    }

    lua_State* state = (lua_State*)lua_runtime_get_state(context->runtime);
    lua_getglobal(state, "result");
    const char* text = lua_tostring(state, -1);
    char result[1024];
    result[0] = '\0';
    if (text != NULL) {
        snprintf(result, sizeof(result), "%s", text);
    }
    lua_pop(state, 1);

    LOG_I(
        "LuaRunner",
        "script evaluated: %s (%u-byte result, %u content children)",
        context->path,
        (unsigned)strlen(result),
        (unsigned)lv_obj_get_child_count(content)
    );

    if (lv_obj_get_child_count(content) == 0) {
        show_text(
            content,
            result[0] != '\0' ? result : "(script created no widgets and set no result)",
            0xc0caf5
        );
    }
}

/*
 * A packaged Lua app ships this runner as its binary plus a lua/main.lua. There
 * is no argument in that case, so resolve the script from our own install
 * directory. While an app starts, its own instance is the topmost one, which is
 * how we learn which manifest to ask about.
 */
static bool resolve_packaged_app(
    char* out_path,
    size_t path_size,
    char* out_name,
    size_t name_size,
    char* out_dir,
    size_t dir_size
) {
    AppId app_id;
    if (app_manager_get_topmost_app_id(app_id, sizeof(app_id)) != ERROR_NONE) {
        LOG_W("LuaRunner", "could not resolve the current app id");
        return false;
    }

    char install_dir[FILE_MAX_PATH_STRING_LENGTH];
    if (app_paths_get_install_directory(app_id, install_dir, sizeof(install_dir)) != ERROR_NONE) {
        LOG_W("LuaRunner", "%s is not an installed package", app_id);
        return false;
    }

    const int written = snprintf(out_path, path_size, "%s/lua/main.lua", install_dir);
    if (written <= 0 || (size_t)written >= path_size) {
        return false;
    }

    snprintf(out_dir, dir_size, "%s", install_dir);

    struct AppManifest manifest;
    if (app_manager_find_manifest(app_id, &manifest) == ERROR_NONE) {
        snprintf(out_name, name_size, "%s", manifest.name);
    } else {
        snprintf(out_name, name_size, "%s", "Lua");
    }

    LOG_I("LuaRunner", "packaged app %s: %s", app_id, out_path);
    return true;
}

int main(int argc, char* argv[]) {
    const char* path = (argc > 0) ? argv[0] : NULL;

    char name[APP_MANIFEST_NAME_LENGTH + 1] = "Lua";
    char script_arg[FILE_MAX_PATH_STRING_LENGTH] = "";
    char packaged_path[FILE_MAX_PATH_STRING_LENGTH];
    bool packaged = false;
    int required_api = LUA_RUNTIME_API_VERSION;

    if (path == NULL) {
        packaged = resolve_packaged_app(
            packaged_path,
            sizeof(packaged_path),
            name,
            sizeof(name),
            script_arg,
            sizeof(script_arg)
        );
        if (packaged) {
            path = packaged_path;
            required_api = read_required_api(script_arg);
        }
    }

    if (path != NULL && !packaged) {
        const char* slash = strrchr(path, '/');
        const char* base = (slash != NULL) ? slash + 1 : path;

        size_t name_length = strlen(base);
        if (name_length > 4 && strcmp(base + name_length - 4, ".lua") == 0) {
            name_length -= 4;
        }
        if (name_length > APP_MANIFEST_NAME_LENGTH) {
            name_length = APP_MANIFEST_NAME_LENGTH;
        }
        memcpy(name, base, name_length);
        name[name_length] = '\0';
        if (name[0] >= 'a' && name[0] <= 'z') {
            name[0] = (char)(name[0] - ('a' - 'A'));
        }

        if (slash != NULL) {
            size_t directory_length = (size_t)(slash - path);
            if (directory_length >= sizeof(script_arg)) {
                directory_length = sizeof(script_arg) - 1;
            }
            memcpy(script_arg, path, directory_length);
            script_arg[directory_length] = '\0';
        }
    }

    LOG_I("LuaRunner", "started: %s", path != NULL ? path : "(no path)");

    AppInstanceId app_instance_id = app_scheduler_current_app_id();

    struct ScriptContext context = {
        .path = path,
        .name = name,
        .script_arg = script_arg,
        .required_api = required_api,
        .app_instance_id = app_instance_id,
    };

    struct TaskEventGroup event_group = { 0 };
    task_event_group_construct(&event_group);

    struct AppEventSubscription sub = { 0 };
    check(app_event_subscribe(&sub, &event_group) == ERROR_NONE);

    WindowId window = window_manager_create(app_instance_id, create_widgets, &context);

    bool should_close = false;
    while (!should_close) {
        // A timeout rather than portMAX_DELAY: picker results are processed here, on
        // the app task, because starting or stopping apps from the LVGL task deadlocks.
        task_event_group_wait_any(&event_group, NULL, pdMS_TO_TICKS(200));

        struct AppEvent event;
        while (app_event_poll(&sub, &event) == ERROR_NONE) {
            if (event.type == APP_EVENT_CLOSE) {
                should_close = true;
                break;
            }
        }

        if (!should_close && context.runtime != NULL && context.luavgl_open) {
            lvgl_lock();
            luavgl_pickers_poll(context.runtime);
            lvgl_unlock();
        }
    }

    if (context.runtime != NULL) {
        lvgl_lock();
        call_optional(context.runtime, "on_hide");
        call_optional(context.runtime, "on_destroy");
        if (context.luavgl_open) {
            luavgl_bindings_close(context.runtime);
        }
        lvgl_unlock();
        lua_runtime_free(context.runtime);
        context.runtime = NULL;
    }

    window_manager_remove(window);
    check(app_event_unsubscribe(&sub) == ERROR_NONE);
    task_event_group_destruct(&event_group);

    LOG_I("LuaRunner", "stopped");
    return 0;
}
