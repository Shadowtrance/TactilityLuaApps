// SPDX-License-Identifier: Apache-2.0
/*
 * Lua Apps: manage the packaged Lua applications on this device.
 *
 * The Lua runtime ships a companion app for scripts, but every packaged Lua app
 * is a separate launcher entry. This lists those packages (id prefix
 * "tactility.lua.") in one place, opens them and uninstalls them.
 *
 * It is a normal Tactility app in the same package as the Lua runtime, so it
 * does not need the interpreter itself: it talks to the app manager directly.
 */
#include <app/event.h>
#include <app/install.h>
#include <app/manager.h>
#include <app/manifest.h>
#include <app/paths.h>
#include <app/scheduler.h>
#include <app/start.h>

#include <lvgl.h>
#include <lvgl/fonts.h>
#include <lvgl/icons/shared.h>
#include <lvgl/lvgl.h>
#include <lvgl/svg.h>
#include <lvgl/theme.h>
#include <lvgl/widgets/card.h>
#include <lvgl/widgets/icon_button.h>
#include <lvgl/widgets/list.h>
#include <lvgl/widgets/text_button.h>
#include <lvgl/widgets/toolbar.h>
#include <lvgl_window_manager/window_manager.h>

#include <tactility/check.h>
#include <tactility/concurrent/task_event_group.h>
#include <tactility/error.h>
#include <tactility/log.h>
#include <tactility/paths.h>

#include <strings.h>
#include <sys/stat.h>

#include <algorithm>
#include <atomic>
#include <string>
#include <vector>

namespace {

constexpr auto* TAG = "LuaApps";

/** Packaged Lua apps use this id prefix, see tools/lua-app/make_lua_app.py. */
constexpr auto* LUA_APP_ID_PREFIX = "tactility.lua.";
/** This app itself, which is in the package but not a Lua app. */
constexpr auto* SELF_APP_ID = "tactility.luaapps";
/** The app that edits and packages scripts, offered from the empty state. */
constexpr auto* LUA_COMPANION_APP_ID = "tactility.luaruntime";

/** Corner radius of a 96 px launcher icon, scaled to the row icon size. */
constexpr uint32_t ICON_CORNER_RADIUS = 22;

struct LuaApp {
    ::std::string id;
    ::std::string name;
    ::std::string version;
    lv_draw_buf_t* icon = nullptr;
    bool icon_is_mono = false;
};

struct Context {
    AppInstanceId instance_id = 0;
    AppEventSubscription subscription {};
    TaskEventGroup* event_group = nullptr;
    uint32_t uninstall_bit = 0;
    uint32_t start_bit = 0;

    ::std::vector<LuaApp> apps;
    ::std::string pending_uninstall;
    ::std::atomic<bool> uninstall_requested { false };
    ::std::string pending_start;
    ::std::atomic<bool> start_requested { false };

    /** The root passed to createWidgets(); the dialogs live here. */
    lv_obj_t* window_root = nullptr;
    /** The padded content column that holds the list. */
    lv_obj_t* content = nullptr;
    lv_obj_t* overlay = nullptr;

    ~Context() {
        for (auto& app : apps) {
            if (app.icon != nullptr) {
                lv_draw_buf_destroy(app.icon);
            }
        }
    }
};

// Data -------------------------------------------------------------------------------

/**
 * Collects the installed Lua apps. Runs on the app's own task: it reads
 * manifests and install directories, which the LVGL thread must not do.
 *
 * Icons of apps that are still present are moved over, so a refresh does not
 * rasterize every icon again.
 */
void collect_apps(Context& context) {
    struct Package {
        ::std::string version;
        ::std::vector<::std::string> app_ids;
    };

    ::std::vector<Package> packages;
    app_manager_for_each_package(
        [](const struct AppPackage* package, void* user_data) {
            auto& out = *static_cast<::std::vector<Package>*>(user_data);
            Package entry;
            entry.version = package->package.version_name;
            for (size_t i = 0; i < package->app_id_count; i++) {
                entry.app_ids.emplace_back(package->app_ids[i]);
            }
            out.push_back(::std::move(entry));
        },
        &packages
    );

    ::std::vector<LuaApp> found;
    for (const auto& package : packages) {
        for (const auto& app_id : package.app_ids) {
            if (app_id.rfind(LUA_APP_ID_PREFIX, 0) != 0 || app_id == SELF_APP_ID) {
                continue;
            }
            struct AppManifest manifest {};
            if (app_manager_find_manifest(app_id.c_str(), &manifest) != ERROR_NONE) {
                continue;
            }
            if ((manifest.flags & APP_MANIFEST_FLAG_HIDDEN) != 0) {
                continue;
            }
            found.push_back({ app_id, manifest.name, package.version });
        }
    }

    ::std::sort(found.begin(), found.end(), [](const LuaApp& left, const LuaApp& right) {
        return strcasecmp(left.name.c_str(), right.name.c_str()) < 0;
    });

    for (auto& app : found) {
        for (auto& previous : context.apps) {
            if (previous.id == app.id) {
                app.icon = previous.icon;
                app.icon_is_mono = previous.icon_is_mono;
                previous.icon = nullptr;
                break;
            }
        }
    }
    context.apps = ::std::move(found);
}

/** Rasterizes the package's icon. File I/O: app task only. */
bool load_icon(LuaApp& app, uint32_t size) {
    char install_dir[FILE_MAX_PATH_STRING_LENGTH];
    if (app_paths_get_install_directory(app.id.c_str(), install_dir, sizeof(install_dir)) != ERROR_NONE) {
        return false;
    }

    const ::std::string colour_path = ::std::string(install_dir) + "/icon.svg";
    const ::std::string mono_path = ::std::string(install_dir) + "/icon_mono.svg";
    struct stat info {};
    const bool has_colour = stat(colour_path.c_str(), &info) == 0;
    const bool has_mono = stat(mono_path.c_str(), &info) == 0;
    if (!has_colour && !has_mono) {
        return false;
    }

    bool is_mono = false;
    app.icon = lvgl_svg_rasterize_file_variants(
        has_colour ? colour_path.c_str() : nullptr,
        has_mono ? mono_path.c_str() : nullptr,
        lvgl_theme_is_mono(),
        size,
        &is_mono
    );
    app.icon_is_mono = is_mono;
    if (app.icon != nullptr && !is_mono) {
        lvgl_svg_round_corners(app.icon, size * ICON_CORNER_RADIUS / 96);
    }
    return app.icon != nullptr;
}

// Widgets ----------------------------------------------------------------------------

void set_icon(lv_obj_t* image, const lv_draw_buf_t* icon, bool recolor) {
    lv_image_set_src(image, icon);
    lv_obj_set_style_image_recolor(image, lv_theme_get_color_primary(image), LV_STATE_DEFAULT);
    lv_obj_set_style_image_recolor_opa(image, recolor ? LV_OPA_COVER : LV_OPA_TRANSP, LV_STATE_DEFAULT);
}

void set_single_line_ellipsis(lv_obj_t* label) {
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(label, LV_PCT(100));
}

void close_dialog(Context* context) {
    if (context->overlay != nullptr) {
        lv_obj_delete(context->overlay);
        context->overlay = nullptr;
    }
}

lv_obj_t* create_button(lv_obj_t* parent, const char* text, lv_event_cb_t callback, void* user_data) {
    auto* button = lv_button_create(parent);
    auto* label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_center(label);
    if (callback != nullptr) {
        lv_obj_add_event_cb(button, callback, LV_EVENT_SHORT_CLICKED, user_data);
    }
    return button;
}

lv_obj_t* create_text_button(lv_obj_t* parent, const char* text, lv_event_cb_t callback, void* user_data) {
    auto* button = lvgl_text_button_create(parent);
    auto* label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_center(label);
    if (callback != nullptr) {
        lv_obj_add_event_cb(button, callback, LV_EVENT_SHORT_CLICKED, user_data);
    }
    return button;
}

/** The modal card both dialogs use. Returns the card; the caller adds the texts and buttons. */
lv_obj_t* create_dialog(Context* context) {
    close_dialog(context);

    auto* overlay = lv_obj_create(context->window_root);
    lv_obj_set_size(overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_align(overlay, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_ignore_layout(overlay, true);
    lv_obj_set_clickable(overlay, true);
    lv_obj_set_style_bg_color(overlay, lv_color_black(), LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_50, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(overlay, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_radius(overlay, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(overlay, 0, LV_STATE_DEFAULT);
    lv_obj_add_event_cb(overlay, [](lv_event_t* event) {
        close_dialog(static_cast<Context*>(lv_event_get_user_data(event)));
    }, LV_EVENT_SHORT_CLICKED, context);
    context->overlay = overlay;

    auto* card = lvgl_card_create(overlay);
    lv_obj_set_width(card, LV_PCT(80));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_center(card);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    return card;
}

void show_message(Context* context, const char* title, const char* message) {
    auto* card = create_dialog(context);
    auto* title_label = lv_label_create(card);
    lv_label_set_text(title_label, title);
    if (message != nullptr) {
        auto* message_label = lv_label_create(card);
        lv_label_set_text(message_label, message);
        lv_obj_set_width(message_label, LV_PCT(100));
        lv_label_set_long_mode(message_label, LV_LABEL_LONG_MODE_WRAP);
    }
    create_button(card, "Close", [](lv_event_t* event) {
        close_dialog(static_cast<Context*>(lv_event_get_user_data(event)));
    }, context);
}

// App actions ------------------------------------------------------------------------

void start_app(const char* app_id) {
    struct AppStartContext start {};
    if (app_start_context_from_id(app_id, &start) != ERROR_NONE) {
        LOG_E(TAG, "App not found: %s", app_id);
        return;
    }
    AppInstanceId instance_id = 0;
    if (app_start_with_context(&start, &instance_id) != ERROR_NONE) {
        LOG_E(TAG, "Failed to start %s", app_id);
    }
}

/**
 * Starting a packaged (flash-loaded) app from the LVGL task trips ESP-IDF's
 * stack-sanity assert while the loader disables the flash cache, so the request
 * is handed to this app's own task.
 */
void request_start(Context* context, const ::std::string& app_id) {
    context->pending_start = app_id;
    context->start_requested = true;
    task_event_group_signal(context->event_group, context->start_bit);
}

void on_app_pressed(lv_event_t* event) {
    auto* context = static_cast<Context*>(lv_event_get_user_data(event));
    auto* widget = lv_event_get_target_obj(event);
    const auto index = reinterpret_cast<intptr_t>(lv_obj_get_user_data(widget));
    if (index >= 0 && static_cast<size_t>(index) < context->apps.size()) {
        request_start(context, context->apps[index].id);
    }
}

void on_confirm_uninstall(lv_event_t* event) {
    auto* context = static_cast<Context*>(lv_event_get_user_data(event));
    auto* widget = lv_event_get_target_obj(event);
    const auto index = reinterpret_cast<intptr_t>(lv_obj_get_user_data(widget));
    if (index < 0 || static_cast<size_t>(index) >= context->apps.size()) {
        return;
    }

    // The app task performs the uninstall off the LVGL thread, then rebuilds the list.
    context->pending_uninstall = context->apps[index].id;
    close_dialog(context);
    context->uninstall_requested = true;
    task_event_group_signal(context->event_group, context->uninstall_bit);
}

void on_uninstall_pressed(lv_event_t* event) {
    auto* context = static_cast<Context*>(lv_event_get_user_data(event));
    auto* widget = lv_event_get_target_obj(event);
    const auto index = reinterpret_cast<intptr_t>(lv_obj_get_user_data(widget));
    if (index < 0 || static_cast<size_t>(index) >= context->apps.size()) {
        return;
    }

    auto* card = create_dialog(context);
    const ::std::string title = "Uninstall " + context->apps[index].name + "?";
    auto* title_label = lv_label_create(card);
    lv_label_set_text(title_label, title.c_str());
    auto* message = lv_label_create(card);
    lv_label_set_text(message, "The app and its files are removed from this device.");
    lv_obj_set_width(message, LV_PCT(100));
    lv_label_set_long_mode(message, LV_LABEL_LONG_MODE_WRAP);

    auto* buttons = lv_obj_create(card);
    lv_obj_remove_style_all(buttons);
    lv_obj_set_size(buttons, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(buttons, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(buttons, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(buttons, 12, LV_STATE_DEFAULT);

    create_text_button(buttons, "Cancel", [](lv_event_t* event) {
        close_dialog(static_cast<Context*>(lv_event_get_user_data(event)));
    }, context);

    auto* confirm = create_button(buttons, "Uninstall", on_confirm_uninstall, context);
    lv_obj_set_user_data(confirm, reinterpret_cast<void*>(static_cast<intptr_t>(index)));
}

void on_open_lua(lv_event_t* event) {
    auto* context = static_cast<Context*>(lv_event_get_user_data(event));
    request_start(context, LUA_COMPANION_APP_ID);
}

void on_close(lv_event_t* event) {
    app_event_emit_close(static_cast<Context*>(lv_event_get_user_data(event))->instance_id);
}

lv_draw_buf_t* on_uninstall_requested(Context* context, const ::std::string& app_id) {
    lv_draw_buf_t* retired_icon = nullptr;
    const auto result = app_uninstall(app_id.c_str());
    if (result == ERROR_NONE) {
        for (auto iterator = context->apps.begin(); iterator != context->apps.end(); ++iterator) {
            if (iterator->id == app_id) {
                // The widgets still show this icon until the list is rebuilt; the
                // caller frees it afterwards.
                retired_icon = iterator->icon;
                context->apps.erase(iterator);
                break;
            }
        }
    }
    context->pending_uninstall.clear();
    if (result != ERROR_NONE) {
        LOG_E(TAG, "Failed to uninstall %s: %d", app_id.c_str(), static_cast<int>(result));
    }
    return retired_icon;
}

// Content ----------------------------------------------------------------------------

void add_row(Context* context, lv_obj_t* list, size_t index, uint32_t icon_size) {
    auto& app = context->apps[index];
    auto* row = lvgl_list_add_button(list, nullptr, nullptr);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    auto* image = lv_image_create(row);
    lv_obj_set_size(image, icon_size, icon_size);
    if (app.icon != nullptr) {
        set_icon(image, app.icon, app.icon_is_mono);
    }

    auto* texts = lv_obj_create(row);
    lv_obj_remove_style_all(texts);
    lv_obj_set_height(texts, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(texts, 1);
    lv_obj_set_flex_flow(texts, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_clickable(texts, false);
    lv_obj_set_scrollable(texts, false);

    auto* name = lv_label_create(texts);
    lv_label_set_text(name, app.name.c_str());
    set_single_line_ellipsis(name);

    auto* meta = lv_label_create(texts);
    const ::std::string meta_text = app.version.empty()
        ? app.id
        : app.id + "  ·  " + app.version;
    lv_label_set_text(meta, meta_text.c_str());
    lv_obj_set_style_text_font(meta, lvgl_get_text_font(FONT_SIZE_SMALL), LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(meta, LV_OPA_70, LV_STATE_DEFAULT);
    set_single_line_ellipsis(meta);

    auto* uninstall = lvgl_icon_button_create_variant(row, LVGL_ICON_BUTTON_STANDARD);
    auto* uninstall_icon = lv_label_create(uninstall);
    lv_label_set_text(uninstall_icon, LVGL_ICON_SHARED_DELETE);
    lv_obj_set_style_text_font(uninstall_icon, lvgl_get_shared_icon_default_font(), LV_STATE_DEFAULT);
    lv_obj_set_user_data(uninstall, reinterpret_cast<void*>(static_cast<intptr_t>(index)));
    lv_obj_add_event_cb(uninstall, on_uninstall_pressed, LV_EVENT_SHORT_CLICKED, context);

    lv_obj_set_user_data(row, reinterpret_cast<void*>(static_cast<intptr_t>(index)));
    lv_obj_add_event_cb(row, on_app_pressed, LV_EVENT_SHORT_CLICKED, context);
}

void build_empty_state(Context* context) {
    auto* card = lvgl_card_create(context->content);
    lv_obj_set_width(card, LV_PCT(100));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    auto* title = lv_label_create(card);
    lv_label_set_text(title, "No Lua apps installed");
    auto* hint = lv_label_create(card);
    lv_label_set_text(hint, "Package a script as a Lua app with the Lua app, or install a .app file.");
    lv_obj_set_width(hint, LV_PCT(100));
    lv_label_set_long_mode(hint, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_font(hint, lvgl_get_text_font(FONT_SIZE_SMALL), LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(hint, LV_OPA_70, LV_STATE_DEFAULT);

    create_text_button(card, "Open Lua", on_open_lua, context);
}

void rebuild_content(Context* context) {
    if (context->content == nullptr) {
        return;
    }
    lv_obj_clean(context->content);

    if (context->apps.empty()) {
        build_empty_state(context);
        return;
    }

    auto* card = lvgl_card_create(context->content);
    lv_obj_set_width(card, LV_PCT(100));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(card, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_clip_corner(card, true, LV_STATE_DEFAULT);

    auto* list = lvgl_list_create(card);
    lv_obj_set_size(list, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(list, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(list, 0, LV_STATE_DEFAULT);

    const uint32_t icon_size = lvgl_get_shared_icon_large_font_height();
    for (size_t i = 0; i < context->apps.size(); i++) {
        add_row(context, list, i, icon_size);
    }
    lvgl_list_hide_last_divider(list);
}

// Window lifecycle -------------------------------------------------------------------

void create_widgets(lv_obj_t* root, void* user_data) {
    auto* context = static_cast<Context*>(user_data);
    context->window_root = root;
    context->overlay = nullptr;

    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(root, 0, LV_STATE_DEFAULT);

    auto* toolbar = lvgl_toolbar_create(root, "Lua Apps");
    lvgl_toolbar_set_nav_action(toolbar, LV_SYMBOL_CLOSE, on_close, context);

    auto* content = lv_obj_create(root);
    lv_obj_set_width(content, LV_PCT(100));
    lv_obj_set_flex_grow(content, 1);
    lv_obj_set_style_border_width(content, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_radius(content, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, LV_STATE_DEFAULT);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    context->content = content;

    rebuild_content(context);
}

void destroy_widgets(void* user_data) {
    auto* context = static_cast<Context*>(user_data);
    // The icons stay alive across a rebuild, so only forget the widget pointers.
    context->content = nullptr;
    context->window_root = nullptr;
    context->overlay = nullptr;
}

extern "C" int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;

    Context context;
    context.instance_id = app_scheduler_current_app_id();

    struct TaskEventGroup event_group {};
    task_event_group_construct(&event_group);
    context.event_group = &event_group;
    check(task_event_group_claim_bit(&event_group, &context.uninstall_bit) == ERROR_NONE);
    check(task_event_group_claim_bit(&event_group, &context.start_bit) == ERROR_NONE);
    check(app_event_subscribe(&context.subscription, &event_group) == ERROR_NONE);

    collect_apps(context);
    const uint32_t icon_size = lvgl_get_shared_icon_large_font_height();
    for (auto& app : context.apps) {
        load_icon(app, icon_size);
    }

    WindowId window = window_manager_create_ext(
        context.instance_id,
        create_widgets,
        destroy_widgets,
        &context
    );

    bool should_close = false;
    while (!should_close) {
        task_event_group_wait_any(&event_group, nullptr, portMAX_DELAY);

        struct AppEvent event {};
        while (app_event_poll(&context.subscription, &event) == ERROR_NONE) {
            if (event.type == APP_EVENT_CLOSE) {
                should_close = true;
            }
        }

        if (!should_close && context.start_requested.exchange(false)) {
            start_app(context.pending_start.c_str());
            context.pending_start.clear();
        }

        if (!should_close && context.uninstall_requested.exchange(false)) {
            lv_draw_buf_t* retired_icon = on_uninstall_requested(&context, context.pending_uninstall);
            lvgl_lock();
            rebuild_content(&context);
            lvgl_unlock();
            if (retired_icon != nullptr) {
                lv_draw_buf_destroy(retired_icon);
            }
        }
    }

    window_manager_remove(window);
    check(app_event_unsubscribe(&context.subscription) == ERROR_NONE);
    task_event_group_destruct(&event_group);
    return 0;
}

} // namespace
