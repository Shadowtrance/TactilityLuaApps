#include <app/event.h>
#include <app/install.h>
#include <app/manager.h>
#include <app/manifest.h>
#include <app/paths.h>
#include <app/scheduler.h>
#include <app/start.h>
#include <app/stream.h>

#include <lua/runtime.h>

#include <lvgl_window_manager/window_manager.h>

#include <tactility/check.h>
#include <tactility/error.h>
#include <tactility/file_ops.h>
#include <tactility/log.h>
#include <tactility/paths.h>

#include <lvgl.h>
#include <lvgl/fonts.h>
#include <lvgl/grid_navigation.h>
#include <lvgl/icons/shared.h>
#include <lvgl/insets.h>
#include <lvgl/lvgl.h>
#include <lvgl/theme.h>
#include <lvgl/widgets/card.h>
#include <lvgl/widgets/icon_button.h>
#include <lvgl/widgets/list.h>
#include <lvgl/widgets/toolbar.h>

#include <dirent.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/** Windows' mkdir takes one argument; POSIX and ESP-IDF take a mode. */
static void make_directory(const char* path) {
#ifdef _WIN32
    mkdir(path);
#else
    mkdir(path, 0777);
#endif
}

/*
 * Lua companion: a themed script browser and editor.
 *
 * Loose scripts are a workshop, not launcher apps. Selecting a script opens it
 * here; Save writes it, Run starts the hidden `lua.runner` app with the path.
 * The runner owns the Lua runtime and the luavgl bindings for the script.
 *
 * The package's bundled examples are listed directly from its assets, so they
 * do not have to be copied. Opening one is harmless: Run executes the asset
 * as-is, as long as it is untouched. Saving, or running after an edit, writes
 * a copy into the selected folder first and continues from that copy.
 */

#define PACKAGE_ID "tactility.luaruntime"

#define MAX_ENTRIES 16
#define MAX_EDITOR_BYTES (64 * 1024)
#define MAX_FILTER_LENGTH 48

enum ScriptSource {
    SCRIPT_SOURCE_EXAMPLE,
    SCRIPT_SOURCE_FOLDER,
};

enum DialogKind {
    DIALOG_NONE,
    DIALOG_FOLDER,
    DIALOG_NEW_FILE,
    DIALOG_EXPORT_NAME,
};

struct Context;

struct ScriptEntry {
    char path[FILE_MAX_PATH_STRING_LENGTH];
    char name[APP_MANIFEST_NAME_LENGTH + 1];
    enum ScriptSource source;
    struct Context* context;
};

struct Context {
    char script_dir[FILE_MAX_PATH_STRING_LENGTH];
    char examples_dir[FILE_MAX_PATH_STRING_LENGTH];
    /* Case-insensitive substring filter for the browser lists. */
    char filter[MAX_FILTER_LENGTH + 1];

    struct ScriptEntry entries[MAX_ENTRIES];
    size_t entry_count;
    struct ScriptEntry examples[MAX_ENTRIES];
    size_t example_count;

    lv_obj_t* content;
    lv_obj_t* toolbar;
    lv_obj_t* overflow;
    lv_obj_t* title_label;
    lv_obj_t* position_label;
    lv_obj_t* save_label;
    lv_obj_t* status_label;
    lv_obj_t* editor;
    lv_obj_t* search_row;
    lv_obj_t* search_textarea;
    lv_obj_t* list_container;
    bool search_visible;
    /* Suppresses the query handler while the filter is set programmatically. */
    bool suppress_query;
    char current_path[FILE_MAX_PATH_STRING_LENGTH];
    /* The name dialog hides and rebuilds this app, so the export target is captured first. */
    char pending_export_path[FILE_MAX_PATH_STRING_LENGTH];
    bool editing_example;
    /* Guards the VALUE_CHANGED handler while it inserts indentation. */
    bool auto_indent_guard;
    /* Hash of the bundled example as opened, so Run can tell an untouched one. */
    uint64_t example_hash;
    char message[128];

    struct TaskEventGroup* event_group;
    AppInstanceId app_instance_id;
    struct AppStream dialog_stream;
    uint8_t dialog_buffer[256];
    uint32_t dialog_launch_id;
    enum DialogKind dialog_kind;

    /* Export runs on the file-ops task; the app thread polls these. */
    volatile bool export_running;
    volatile bool export_done;
    char export_message[128];
};

struct StartScriptJob {
    char path[FILE_MAX_PATH_STRING_LENGTH];
};

struct DialogJob {
    struct Context* context;
    enum DialogKind kind;
    char prefill[APP_MANIFEST_NAME_LENGTH + 1];
};

static void show_browser(struct Context* context);
static void show_editor(struct Context* context, struct ScriptEntry* script);
static const char* file_name(const char* path);
static void script_stem(const char* path, char* out_stem, size_t size);

static void set_status(struct Context* context, const char* message) {
    if (context->status_label != NULL) {
        lv_label_set_text(context->status_label, message);
    }
    LOG_I("Lua", "%s", message);
}

static void set_message(struct Context* context, const char* message) {
    snprintf(context->message, sizeof(context->message), "%s", message);
}

/** The root that holds the user's folders: /sdcard or /data. */
static bool get_storage_root(char* out_path, size_t out_size) {
    char data_path[FILE_MAX_PATH_STRING_LENGTH];
    if (paths_get_data_path(data_path, sizeof(data_path)) != ERROR_NONE) {
        return false;
    }

    // The data path is <mount>/tactility, so the storage root is the first segment.
    const char* separator = strchr(data_path + 1, '/');
    const size_t length = separator != NULL ? (size_t)(separator - data_path) : strlen(data_path);
    if (length == 0 || length >= out_size) {
        return false;
    }

    memcpy(out_path, data_path, length);
    out_path[length] = '\0';
    return true;
}

/** Where scripts live when the user has not picked another folder. */
static bool resolve_script_dir(char* out_path, size_t out_size) {
    char storage_root[FILE_MAX_PATH_STRING_LENGTH];
    if (!get_storage_root(storage_root, sizeof(storage_root))) {
        return false;
    }

    const int written = snprintf(out_path, out_size, "%s/lua", storage_root);
    return written > 0 && (size_t)written < out_size;
}

/** The companion's own settings file, in <data>/appdata/tactility.luaruntime/. */
static bool get_config_path(char* out_path, size_t out_size) {
    return app_paths_get_user_data_path(PACKAGE_ID, "runtime.properties", out_path, out_size) == ERROR_NONE;
}

/** Reads script_dir from a properties file. Leaves out_path untouched when there is none. */
static bool read_script_dir_from(const char* config_path, char* out_path, size_t out_size) {
    FILE* file = fopen(config_path, "r");
    if (file == NULL) {
        return false;
    }

    bool found = false;
    char line[FILE_MAX_PATH_STRING_LENGTH + 32];
    while (fgets(line, sizeof(line), file) != NULL) {
        const char* prefix = "script_dir=";
        if (strncmp(line, prefix, strlen(prefix)) != 0) {
            continue;
        }

        const char* value = line + strlen(prefix);
        size_t length = strlen(value);
        while (length > 0 && (value[length - 1] == '\n' || value[length - 1] == '\r')) {
            length--;
        }
        if (length > 0 && length < out_size) {
            memcpy(out_path, value, length);
            out_path[length] = '\0';
            found = true;
        }
        break;
    }

    fclose(file);
    return found;
}

static void load_script_dir(char* out_path, size_t out_size) {
    out_path[0] = '\0';
    if (!resolve_script_dir(out_path, out_size)) {
        return;
    }

    char config_path[FILE_MAX_PATH_STRING_LENGTH];
    if (get_config_path(config_path, sizeof(config_path))) {
        read_script_dir_from(config_path, out_path, out_size);
    }
}

static void save_script_dir(const char* path) {
    char config_path[FILE_MAX_PATH_STRING_LENGTH];
    if (!get_config_path(config_path, sizeof(config_path))) {
        return;
    }

    char directory[FILE_MAX_PATH_STRING_LENGTH];
    snprintf(directory, sizeof(directory), "%s", config_path);
    char* separator = strrchr(directory, '/');
    if (separator != NULL) {
        *separator = '\0';
        make_directory(directory);
    }

    FILE* file = fopen(config_path, "w");
    if (file == NULL) {
        LOG_E("Lua", "could not write %s", config_path);
        return;
    }

    fprintf(file, "script_dir=%s\n", path);
    fclose(file);
}

/** Resolves the package's bundled examples folder, or an empty path if there is none. */
static void load_examples_dir(char* out_path, size_t out_size) {
    out_path[0] = '\0';

    char assets[FILE_MAX_PATH_STRING_LENGTH];
    if (app_paths_get_assets_directory(PACKAGE_ID, assets, sizeof(assets)) != ERROR_NONE) {
        return;
    }

    const int written = snprintf(out_path, out_size, "%s/examples", assets);
    if (written <= 0 || (size_t)written >= out_size) {
        out_path[0] = '\0';
        return;
    }

    DIR* directory = opendir(out_path);
    if (directory == NULL) {
        out_path[0] = '\0';
        return;
    }
    closedir(directory);
}

static bool contains_ignore_case(const char* text, const char* needle) {
    if (needle[0] == '\0') {
        return true;
    }

    const size_t needle_length = strlen(needle);
    for (const char* start = text; *start != '\0'; start++) {
        size_t index = 0;
        while (
            index < needle_length && start[index] != '\0' &&
            tolower((unsigned char)start[index]) == tolower((unsigned char)needle[index])
        ) {
            index++;
        }
        if (index == needle_length) {
            return true;
        }
    }
    return false;
}

static size_t collect_lua_files(
    struct Context* context,
    const char* directory_path,
    struct ScriptEntry* entries,
    size_t capacity,
    const char* filter,
    enum ScriptSource source
) {
    if (directory_path[0] == '\0') {
        return 0;
    }

    DIR* directory = opendir(directory_path);
    if (directory == NULL) {
        return 0;
    }

    size_t count = 0;
    struct dirent* entry;
    while (count < capacity && (entry = readdir(directory)) != NULL) {
        const char* name = entry->d_name;
        const size_t name_length = strlen(name);
        if (name_length <= 4 || name_length > 64 || strcmp(name + name_length - 4, ".lua") != 0) {
            continue;
        }

        const size_t stem_length = name_length - 4;
        if (stem_length > APP_MANIFEST_NAME_LENGTH) {
            continue;
        }
        if (filter != NULL && !contains_ignore_case(name, filter)) {
            continue;
        }

        struct ScriptEntry* script = &entries[count];
        const int path_length = snprintf(script->path, sizeof(script->path), "%s/%s", directory_path, name);
        if (path_length <= 0 || (size_t)path_length >= sizeof(script->path)) {
            continue;
        }

        memcpy(script->name, name, stem_length);
        script->name[stem_length] = '\0';
        if (script->name[0] >= 'a' && script->name[0] <= 'z') {
            script->name[0] = (char)(script->name[0] - ('a' - 'A'));
        }

        script->source = source;
        script->context = context;
        count++;
    }

    closedir(directory);
    return count;
}

static void start_script_job(void* context) {
    struct StartScriptJob* job = (struct StartScriptJob*)context;
    const char* argv[] = { job->path, NULL };

    AppInstanceId instance_id = 0;
    error_t error = app_start("lua.runner", 1, argv, &instance_id);
    if (error != ERROR_NONE) {
        LOG_E("Lua", "failed to start %s: %s", job->path, error_to_string(error));
    }

    free(job);
}

/*
 * Starting another app from an LVGL callback deadlocks: the loader stops the
 * current app and takes the LVGL lock, which the LVGL task holds for the
 * callback's duration. Queue it on the file operations task instead, like the
 * launcher does.
 */
static void queue_run(const char* path) {
    struct StartScriptJob* job = (struct StartScriptJob*)malloc(sizeof(struct StartScriptJob));
    if (job == NULL) {
        return;
    }

    snprintf(job->path, sizeof(job->path), "%s", path);
    if (file_ops_queue(start_script_job, job, NULL) != ERROR_NONE) {
        free(job);
    }
}

static void launch_dialog_job(void* argument) {
    struct DialogJob* job = (struct DialogJob*)argument;
    struct Context* context = job->context;
    const enum DialogKind kind = job->kind;
    char prefill[APP_MANIFEST_NAME_LENGTH + 1];
    snprintf(prefill, sizeof(prefill), "%s", job->prefill);
    free(job);

    struct AppStartContext start;
    if (kind == DIALOG_FOLDER) {
        if (app_start_context_from_id("tactility.fileselection", &start) != ERROR_NONE) {
            LOG_E("Lua", "file selection app not available");
            return;
        }
    } else {
        if (app_start_context_from_id("tactility.inputdialog", &start) != ERROR_NONE) {
            LOG_E("Lua", "input dialog app not available");
            return;
        }
    }

    const char* folder_arguments[] = { "--folder", "--no-files", "--max-bytes=256", NULL };
    const char* new_file_arguments[] = { "New script", "File name (no .lua needed)", "", NULL };
    const char* export_arguments[] = { "Make app", "App name", prefill, NULL };
    app_start_context_set_arguments(
        &start,
        kind == DIALOG_FOLDER ? folder_arguments : (kind == DIALOG_EXPORT_NAME ? export_arguments : new_file_arguments)
    );

    struct AppStreamBinding binding = {
        .producer_fd = STDOUT_FILENO,
        .stream = &context->dialog_stream,
        .buffer = context->dialog_buffer,
        .buffer_capacity = sizeof(context->dialog_buffer),
        .event_group = context->event_group,
        .window_size = { 0, 0 },
        .alias_fd = -1,
    };
    app_start_context_set_streams(&start, &binding, 1);
    app_start_context_set_parent(&start, context->app_instance_id);

    uint32_t instance_id = 0;
    if (app_start_with_context(&start, &instance_id) != ERROR_NONE) {
        LOG_E("Lua", "failed to start the dialog app");
        return;
    }

    context->dialog_kind = kind;
    context->dialog_launch_id = instance_id;
}

static void queue_dialog(struct Context* context, enum DialogKind kind, const char* prefill) {
    if (context->dialog_launch_id != 0) {
        return;
    }

    struct DialogJob* job = (struct DialogJob*)malloc(sizeof(struct DialogJob));
    if (job == NULL) {
        return;
    }

    job->context = context;
    job->kind = kind;
    snprintf(job->prefill, sizeof(job->prefill), "%s", prefill != NULL ? prefill : "");
    if (file_ops_queue(launch_dialog_job, job, NULL) != ERROR_NONE) {
        free(job);
    }
}

/*
 * On-device export: a packaged Lua app is a tar with a manifest, icons, the
 * script at lua/main.lua and the runner ELF from this runtime package. Writing
 * the tar here keeps the feature free of firmware changes; the firmware's
 * app_install() does the extraction and registration.
 */

#define TAR_BLOCK_SIZE 512

static bool tar_write_header(FILE* archive, const char* name, unsigned long size, unsigned mode, unsigned long mtime) {
    if (name == NULL || strlen(name) >= 100) {
        return false;
    }

    // The file-ops task has a small stack, and exports are serialized by the app.
    static char header[TAR_BLOCK_SIZE];
    memset(header, 0, sizeof(header));
    memcpy(header, name, strlen(name));
    snprintf(header + 100, 8, "%07o", mode & 07777);
    snprintf(header + 108, 8, "%07o", 0);
    snprintf(header + 116, 8, "%07o", 0);
    snprintf(header + 124, 12, "%011lo", size);
    snprintf(header + 136, 12, "%011lo", mtime);
    memset(header + 148, ' ', 8);
    header[156] = '0';
    memcpy(header + 257, "ustar", 5);
    header[262] = '\0';
    memcpy(header + 263, "00", 2);
    memcpy(header + 265, "root", 4);
    memcpy(header + 297, "root", 4);

    unsigned long checksum = 0;
    for (size_t i = 0; i < sizeof(header); i++) {
        checksum += (unsigned char)header[i];
    }
    snprintf(header + 148, 7, "%06lo", checksum);
    header[155] = ' ';

    return fwrite(header, 1, sizeof(header), archive) == sizeof(header);
}

static bool tar_pad(FILE* archive, size_t size) {
    static const char zeros[TAR_BLOCK_SIZE] = { 0 };
    const size_t remainder = size % TAR_BLOCK_SIZE;
    if (remainder == 0) {
        return true;
    }
    const size_t padding = TAR_BLOCK_SIZE - remainder;
    return fwrite(zeros, 1, padding, archive) == padding;
}

static bool tar_add_data(FILE* archive, const char* name, const void* data, size_t size) {
    if (!tar_write_header(archive, name, (unsigned long)size, 0644, (unsigned long)time(NULL))) {
        return false;
    }
    if (size > 0 && fwrite(data, 1, size, archive) != size) {
        return false;
    }
    return tar_pad(archive, size);
}

static bool tar_add_text(FILE* archive, const char* name, const char* text) {
    return tar_add_data(archive, name, text, strlen(text));
}

static bool tar_add_file(FILE* archive, const char* name, const char* source_path, unsigned mode) {
    struct stat info;
    if (stat(source_path, &info) != 0 || info.st_size < 0) {
        return false;
    }
    const unsigned long size = (unsigned long)info.st_size;
    if (!tar_write_header(archive, name, size, mode, (unsigned long)info.st_mtime)) {
        return false;
    }

    FILE* input = fopen(source_path, "rb");
    if (input == NULL) {
        return false;
    }

    static char buffer[4096];
    unsigned long remaining = size;
    bool success = true;
    while (remaining > 0) {
        const size_t chunk = remaining < sizeof(buffer) ? (size_t)remaining : sizeof(buffer);
        if (fread(buffer, 1, chunk, input) != chunk || fwrite(buffer, 1, chunk, archive) != chunk) {
            success = false;
            break;
        }
        remaining -= chunk;
    }

    fclose(input);
    return success && tar_pad(archive, size);
}

static bool tar_finish(FILE* archive) {
    static const char zeros[TAR_BLOCK_SIZE * 2] = { 0 };
    return fwrite(zeros, 1, sizeof(zeros), archive) == sizeof(zeros);
}

/** Finds the runner ELF installed inside this package, and its platform folder. */
static bool find_runner(char* out_runner, size_t runner_size, char* out_platform, size_t platform_size) {
    static char install_dir[FILE_MAX_PATH_STRING_LENGTH];
    if (app_paths_get_install_directory(PACKAGE_ID, install_dir, sizeof(install_dir)) != ERROR_NONE) {
        return false;
    }

    static char bin_dir[FILE_MAX_PATH_STRING_LENGTH];
    const int bin_length = snprintf(bin_dir, sizeof(bin_dir), "%s/bin", install_dir);
    if (bin_length <= 0 || (size_t)bin_length >= sizeof(bin_dir)) {
        return false;
    }

    DIR* directory = opendir(bin_dir);
    if (directory == NULL) {
        return false;
    }

    bool found = false;
    struct dirent* entry;
    while ((entry = readdir(directory)) != NULL) {
        if (entry->d_name[0] == '.') {
            continue;
        }

        const int written = snprintf(
            out_runner,
            runner_size,
            "%s/%s/lua_runner.elf",
            bin_dir,
            entry->d_name
        );
        if (written <= 0 || (size_t)written >= runner_size) {
            continue;
        }

        if (access(out_runner, F_OK) == 0) {
            snprintf(out_platform, platform_size, "%s", entry->d_name);
            found = true;
            break;
        }
    }

    closedir(directory);
    return found;
}

/** Reads a key from the installed package's manifest.properties. */
static bool read_manifest_value(const char* key, char* out_value, size_t value_size) {
    static char install_dir[FILE_MAX_PATH_STRING_LENGTH];
    if (app_paths_get_install_directory(PACKAGE_ID, install_dir, sizeof(install_dir)) != ERROR_NONE) {
        return false;
    }

    static char path[FILE_MAX_PATH_STRING_LENGTH];
    const int written = snprintf(path, sizeof(path), "%s/manifest.properties", install_dir);
    if (written <= 0 || (size_t)written >= sizeof(path)) {
        return false;
    }

    FILE* file = fopen(path, "r");
    if (file == NULL) {
        return false;
    }

    const size_t key_length = strlen(key);
    bool found = false;
    char line[256];
    while (fgets(line, sizeof(line), file) != NULL) {
        if (strncmp(line, key, key_length) != 0) {
            continue;
        }

        const char* value = line + key_length;
        size_t length = strlen(value);
        while (length > 0 && (value[length - 1] == '\n' || value[length - 1] == '\r')) {
            length--;
        }
        if (length > 0 && length < value_size) {
            memcpy(out_value, value, length);
            out_value[length] = '\0';
            found = true;
        }
        break;
    }

    fclose(file);
    return found;
}

static void slugify(const char* name, char* out_slug, size_t size) {
    size_t length = 0;
    bool last_dash = false;
    for (const char* character = name; *character != '\0' && length + 1 < size; character++) {
        char value = *character;
        if (value >= 'A' && value <= 'Z') {
            value = (char)(value + ('a' - 'A'));
        }

        if ((value >= 'a' && value <= 'z') || (value >= '0' && value <= '9')) {
            out_slug[length++] = value;
            last_dash = false;
        } else if (length > 0 && !last_dash) {
            out_slug[length++] = '-';
            last_dash = true;
        }
    }

    while (length > 0 && out_slug[length - 1] == '-') {
        length--;
    }
    out_slug[length] = '\0';

    if (length == 0) {
        snprintf(out_slug, size, "script");
    }
}

static void script_stem(const char* path, char* out_stem, size_t size) {
    const char* name = file_name(path);
    size_t length = strlen(name);
    if (length > 4 && strcmp(name + length - 4, ".lua") == 0) {
        length -= 4;
    }
    if (length >= size) {
        length = size - 1;
    }
    memcpy(out_stem, name, length);
    out_stem[length] = '\0';
}

struct ExportJob {
    struct Context* context;
    char name[APP_MANIFEST_NAME_LENGTH + 1];
    char script_path[FILE_MAX_PATH_STRING_LENGTH];
};

static void export_app_job(void* argument) {
    struct ExportJob* job = (struct ExportJob*)argument;
    struct Context* context = job->context;

    char message[128] = "Export failed";

    static char runner[FILE_MAX_PATH_STRING_LENGTH];
    static char platform[32];
    if (!find_runner(runner, sizeof(runner), platform, sizeof(platform))) {
        snprintf(message, sizeof(message), "Export failed: runner not found");
        goto finish;
    }

    // These are static because the file-ops task stack is small and one export
    // runs at a time; the app's export flag serializes them.
    static char install_dir[FILE_MAX_PATH_STRING_LENGTH];
    if (app_paths_get_install_directory(PACKAGE_ID, install_dir, sizeof(install_dir)) != ERROR_NONE) {
        snprintf(message, sizeof(message), "Export failed: no install dir");
        goto finish;
    }

    static char icon[FILE_MAX_PATH_STRING_LENGTH + 16];
    static char mono_icon[FILE_MAX_PATH_STRING_LENGTH + 16];
    if (
        snprintf(icon, sizeof(icon), "%s/icon.svg", install_dir) <= 0 ||
        snprintf(mono_icon, sizeof(mono_icon), "%s/icon_mono.svg", install_dir) <= 0
    ) {
        snprintf(message, sizeof(message), "Export failed: path too long");
        goto finish;
    }
    if (access(icon, F_OK) != 0 || access(mono_icon, F_OK) != 0) {
        snprintf(message, sizeof(message), "Export failed: icons missing");
        goto finish;
    }

    static char slug[APP_MANIFEST_ID_LENGTH + 1];
    slugify(job->name, slug, sizeof(slug));

    static char full_id[64];
    snprintf(full_id, sizeof(full_id), "tactility.lua.%s", slug);
    size_t id_length = strlen(full_id);
    if (id_length > APP_MANIFEST_ID_LENGTH) {
        id_length = APP_MANIFEST_ID_LENGTH;
    }
    static char app_id[APP_MANIFEST_ID_LENGTH + 1];
    memcpy(app_id, full_id, id_length);
    app_id[id_length] = '\0';

    static char sdk[32];
    snprintf(sdk, sizeof(sdk), "0.8.0-dev");
    read_manifest_value("target.sdk=", sdk, sizeof(sdk));

    static char data_path[FILE_MAX_PATH_STRING_LENGTH];
    if (paths_get_data_path(data_path, sizeof(data_path)) != ERROR_NONE) {
        snprintf(message, sizeof(message), "Export failed: no data path");
        goto finish;
    }

    static char temp_dir[FILE_MAX_PATH_STRING_LENGTH + 8];
    static char archive_path[FILE_MAX_PATH_STRING_LENGTH + APP_MANIFEST_ID_LENGTH + 16];
    if (
        snprintf(temp_dir, sizeof(temp_dir), "%s/tmp", data_path) <= 0 ||
        snprintf(archive_path, sizeof(archive_path), "%s/%s.app", temp_dir, app_id) <= 0
    ) {
        snprintf(message, sizeof(message), "Export failed: path too long");
        goto finish;
    }
    make_directory(temp_dir);

    static char manifest[1024];
    snprintf(
        manifest,
        sizeof(manifest),
        "manifest.version=0.3\n"
        "target.sdk=%s\n"
        "target.platforms=%s\n"
        "id=%s\n"
        "version.name=0.1.0\n"
        "version.code=1\n"
        "name=%s\n"
        "description=Created on the device from %s\n"
        "app.0.id=%s\n"
        "app.0.name=%s\n"
        "app.0.binary=lua_runner\n"
        "lua.runtime.api=%d\n"
        "requires.ram=1M\n",
        sdk,
        platform,
        app_id,
        job->name,
        file_name(job->script_path),
        app_id,
        job->name,
        LUA_RUNTIME_API_VERSION
    );

    FILE* archive = fopen(archive_path, "wb");
    if (archive == NULL) {
        snprintf(message, sizeof(message), "Export failed: cannot write archive");
        goto finish;
    }

    static char runner_entry[128];
    snprintf(runner_entry, sizeof(runner_entry), "bin/%s/lua_runner.elf", platform);

    const bool written =
        tar_add_text(archive, "manifest.properties", manifest) &&
        tar_add_file(archive, "icon.svg", icon, 0644) &&
        tar_add_file(archive, "icon_mono.svg", mono_icon, 0644) &&
        tar_add_file(archive, "lua/main.lua", job->script_path, 0644) &&
        tar_add_file(archive, runner_entry, runner, 0755) &&
        tar_finish(archive);

    fclose(archive);

    if (!written) {
        remove(archive_path);
        snprintf(message, sizeof(message), "Export failed: archive error");
        goto finish;
    }

    const error_t error = app_install(archive_path);
    remove(archive_path);

    if (error != ERROR_NONE) {
        snprintf(message, sizeof(message), "Export failed: %s", error_to_string(error));
    } else {
        snprintf(message, sizeof(message), "Installed %s", job->name);
    }

finish:
    snprintf(context->export_message, sizeof(context->export_message), "%s", message);
    context->export_done = true;
    context->export_running = false;
    free(job);
}

static void start_export(struct Context* context, const char* name) {
    if (context->export_running) {
        set_status(context, "An export is already running");
        return;
    }

    while (*name == ' ' || *name == '\t') {
        name++;
    }
    if (*name == '\0') {
        set_status(context, "App name required");
        return;
    }
    LOG_I("Lua", "export request: name=%s path=%s", name, context->pending_export_path);
    if (context->pending_export_path[0] == '\0') {
        set_status(context, "No script to export");
        return;
    }

    struct ExportJob* job = (struct ExportJob*)malloc(sizeof(struct ExportJob));
    if (job == NULL) {
        set_status(context, "Export failed: out of memory");
        return;
    }

    job->context = context;
    snprintf(job->name, sizeof(job->name), "%s", name);
    snprintf(job->script_path, sizeof(job->script_path), "%s", context->pending_export_path);
    context->pending_export_path[0] = '\0';

    context->export_running = true;
    context->export_done = false;
    context->export_message[0] = '\0';

    if (file_ops_queue(export_app_job, job, NULL) != ERROR_NONE) {
        free(job);
        context->export_running = false;
        set_status(context, "Export failed: queue full");
        return;
    }

    set_status(context, "Exporting...");
}

static char* read_file(const char* path) {
    FILE* file = fopen(path, "rb");
    if (file == NULL) {
        return NULL;
    }

    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return NULL;
    }
    const long size = ftell(file);
    if (size < 0 || size > MAX_EDITOR_BYTES) {
        fclose(file);
        return NULL;
    }
    rewind(file);

    char* text = (char*)malloc((size_t)size + 1);
    if (text == NULL) {
        fclose(file);
        return NULL;
    }

    const size_t read = fread(text, 1, (size_t)size, file);
    fclose(file);
    text[read] = '\0';
    return text;
}

static bool write_file(const char* path, const char* text) {
    FILE* file = fopen(path, "wb");
    if (file == NULL) {
        return false;
    }

    const size_t length = strlen(text);
    const bool written = fwrite(text, 1, length, file) == length;
    fclose(file);
    return written;
}

/** 64-bit FNV-1a: cheap identity for "the editor still matches the example". */
static uint64_t hash_text(const char* text) {
    uint64_t hash = 1469598103934665603ULL;
    for (const unsigned char* current = (const unsigned char*)text; *current != '\0'; current++) {
        hash ^= *current;
        hash *= 1099511628211ULL;
    }
    return hash;
}

/*
 * A bundled example is read-only. Save, or Run after an edit, writes the
 * editor's content into the selected folder and continues from that copy, so
 * a user's edits never land back in the package.
 */
static bool save_example_copy(struct Context* context) {
    if (context->editor == NULL) {
        return false;
    }

    make_directory(context->script_dir);

    const char* name = file_name(context->current_path);
    size_t stem_length = strlen(name);
    if (stem_length > 4 && strcmp(name + stem_length - 4, ".lua") == 0) {
        stem_length -= 4;
    }
    if (stem_length == 0 || stem_length > APP_MANIFEST_NAME_LENGTH) {
        return false;
    }

    char stem[APP_MANIFEST_NAME_LENGTH + 1];
    memcpy(stem, name, stem_length);
    stem[stem_length] = '\0';

    char destination[FILE_MAX_PATH_STRING_LENGTH];
    bool found = false;
    for (int index = 1; index < 100 && !found; index++) {
        const int written = index == 1
            ? snprintf(destination, sizeof(destination), "%s/%s.lua", context->script_dir, stem)
            : snprintf(destination, sizeof(destination), "%s/%s-%d.lua", context->script_dir, stem, index);
        if (written <= 0 || (size_t)written >= sizeof(destination)) {
            return false;
        }
        found = access(destination, F_OK) != 0;
    }
    if (!found) {
        return false;
    }

    if (!write_file(destination, lv_textarea_get_text(context->editor))) {
        return false;
    }

    snprintf(context->current_path, sizeof(context->current_path), "%s", destination);
    context->editing_example = false;

    if (context->title_label != NULL) {
        lv_label_set_text(context->title_label, file_name(destination));
    }
    if (context->toolbar != NULL) {
        lvgl_toolbar_set_title(context->toolbar, file_name(destination));
    }
    if (context->save_label != NULL) {
        lv_label_set_text(context->save_label, LVGL_ICON_SHARED_SAVE);
    }

    char message[128];
    snprintf(message, sizeof(message), "Saved as %s", file_name(destination));
    set_status(context, message);
    return true;
}

static void on_save(lv_event_t* event) {
    struct Context* context = (struct Context*)lv_event_get_user_data(event);
    if (context->editor == NULL) {
        return;
    }

    if (context->editing_example) {
        if (!save_example_copy(context)) {
            set_status(context, "Save failed");
        }
        return;
    }

    const char* text = lv_textarea_get_text(context->editor);
    if (write_file(context->current_path, text)) {
        set_status(context, "Saved");
    } else {
        set_status(context, "Save failed");
    }
}

static void on_run(lv_event_t* event) {
    struct Context* context = (struct Context*)lv_event_get_user_data(event);
    if (context->editor == NULL) {
        return;
    }

    if (context->editing_example) {
        const char* text = lv_textarea_get_text(context->editor);
        if (hash_text(text) == context->example_hash) {
            /* Untouched: run the package asset as-is instead of littering the
             * user's folder with numbered copies for a quick demo. */
            set_status(context, "Running...");
            queue_run(context->current_path);
            return;
        }

        if (!save_example_copy(context)) {
            set_status(context, "Save failed");
            return;
        }
    } else {
        const char* text = lv_textarea_get_text(context->editor);
        if (!write_file(context->current_path, text)) {
            set_status(context, "Save failed");
            return;
        }
    }

    set_status(context, "Running...");
    queue_run(context->current_path);
}

static void on_back(lv_event_t* event) {
    show_browser((struct Context*)lv_event_get_user_data(event));
}

/** Saves the editor's content, then asks for the name of the app to package it as. */
static void on_export(lv_event_t* event) {
    struct Context* context = (struct Context*)lv_event_get_user_data(event);
    if (context->editor == NULL) {
        return;
    }

    if (context->editing_example) {
        if (!save_example_copy(context)) {
            set_status(context, "Save failed");
            return;
        }
    } else if (!write_file(context->current_path, lv_textarea_get_text(context->editor))) {
        set_status(context, "Save failed");
        return;
    }

    snprintf(context->pending_export_path, sizeof(context->pending_export_path), "%s", context->current_path);

    char stem[APP_MANIFEST_NAME_LENGTH + 1];
    script_stem(context->current_path, stem, sizeof(stem));
    queue_dialog(context, DIALOG_EXPORT_NAME, stem);
}

static void on_folder(lv_event_t* event) {
    queue_dialog((struct Context*)lv_event_get_user_data(event), DIALOG_FOLDER, NULL);
}

static void on_new(lv_event_t* event) {
    queue_dialog((struct Context*)lv_event_get_user_data(event), DIALOG_NEW_FILE, NULL);
}

static const char* file_name(const char* path) {
    const char* slash = strrchr(path, '/');
    return slash != NULL ? slash + 1 : path;
}

/** Turns user input into a plain file stem: trimmed, no ".lua", no path separators. */
static bool normalize_new_name(const char* text, char* out_name, size_t out_size) {
    while (*text == ' ' || *text == '\t' || *text == '\n' || *text == '\r') {
        text++;
    }

    size_t length = strlen(text);
    while (length > 0 && (
        text[length - 1] == ' ' || text[length - 1] == '\t' ||
        text[length - 1] == '\n' || text[length - 1] == '\r'
    )) {
        length--;
    }

    if (length > 4 && strcmp(text + length - 4, ".lua") == 0) {
        length -= 4;
    }

    if (length == 0 || length >= out_size) {
        return false;
    }

    for (size_t i = 0; i < length; i++) {
        const char character = text[i];
        const bool valid =
            (character >= 'a' && character <= 'z') ||
            (character >= 'A' && character <= 'Z') ||
            (character >= '0' && character <= '9') ||
            character == '-' || character == '_' || character == ' ';
        if (!valid) {
            return false;
        }
    }

    memcpy(out_name, text, length);
    out_name[length] = '\0';
    return true;
}

static void create_new_script(struct Context* context, const char* requested_name) {
    char name[APP_MANIFEST_NAME_LENGTH + 1];
    if (!normalize_new_name(requested_name, name, sizeof(name))) {
        set_message(context, "Invalid file name");
        show_browser(context);
        return;
    }

    make_directory(context->script_dir);

    struct ScriptEntry script = { 0 };
    const int path_length = snprintf(script.path, sizeof(script.path), "%s/%s.lua", context->script_dir, name);
    if (path_length <= 0 || (size_t)path_length >= sizeof(script.path)) {
        set_message(context, "File name is too long");
        show_browser(context);
        return;
    }

    if (access(script.path, F_OK) == 0) {
        char message[128];
        snprintf(message, sizeof(message), "Already exists: %s.lua", name);
        set_message(context, message);
        show_browser(context);
        return;
    }

    char header[128];
    snprintf(header, sizeof(header), "-- %s.lua\n\n", name);
    if (!write_file(script.path, header)) {
        set_message(context, "Could not create the file");
        show_browser(context);
        return;
    }

    snprintf(script.name, sizeof(script.name), "%s", name);
    script.source = SCRIPT_SOURCE_FOLDER;
    script.context = context;

    show_editor(context, &script);
    if (context->editor != NULL) {
        lv_textarea_set_cursor_pos(context->editor, LV_TEXTAREA_CURSOR_LAST);
        lv_group_focus_obj(context->editor);
    }
}

/** Adds an icon-font action to the companion's toolbar. */
static lv_obj_t* add_toolbar_action(struct Context* context, const char* icon, lv_event_cb_t callback) {
    lv_obj_t* button = lvgl_toolbar_add_text_button_action(context->toolbar, icon, callback, context);
    lv_obj_t* label = lv_obj_get_child(button, 0);
    if (label != NULL) {
        lv_obj_set_style_text_font(label, lvgl_get_shared_icon_default_font(), 0);
    }
    return button;
}

static void set_overflow_visible(struct Context* context, bool visible) {
    if (context->overflow == NULL) {
        return;
    }
    lv_obj_set_hidden(context->overflow, !visible);
    if (visible) {
        lv_obj_align_to(context->overflow, context->toolbar, LV_ALIGN_OUT_BOTTOM_MID, 0, 0);
    }
}

static void clear_overflow(struct Context* context) {
    if (context->overflow != NULL) {
        lv_obj_clean(context->overflow);
        lv_obj_set_hidden(context->overflow, true);
    }
}

static lv_obj_t* add_overflow_button(struct Context* context, const char* icon, lv_event_cb_t callback) {
    lv_obj_t* button = lvgl_icon_button_create(context->overflow);
    lv_obj_t* label = lv_label_create(button);
    lv_obj_set_style_text_font(label, lvgl_get_shared_icon_default_font(), 0);
    lv_label_set_text(label, icon);
    lv_obj_add_event_cb(button, callback, LV_EVENT_SHORT_CLICKED, context);
    return button;
}

static void on_overflow(lv_event_t* event) {
    struct Context* context = (struct Context*)lv_event_get_user_data(event);
    set_overflow_visible(context, lv_obj_is_hidden(context->overflow));
}

static void on_overflow_key(lv_event_t* event) {
    if (lv_event_get_key(event) == LV_KEY_ESC) {
        set_overflow_visible((struct Context*)lv_event_get_user_data(event), false);
    }
}

static void on_folder_from_menu(lv_event_t* event) {
    set_overflow_visible((struct Context*)lv_event_get_user_data(event), false);
    on_folder(event);
}

static void on_new_from_menu(lv_event_t* event) {
    set_overflow_visible((struct Context*)lv_event_get_user_data(event), false);
    on_new(event);
}

static void on_save_from_menu(lv_event_t* event) {
    set_overflow_visible((struct Context*)lv_event_get_user_data(event), false);
    on_save(event);
}

static void on_run_from_menu(lv_event_t* event) {
    set_overflow_visible((struct Context*)lv_event_get_user_data(event), false);
    on_run(event);
}

static void on_export_from_menu(lv_event_t* event) {
    set_overflow_visible((struct Context*)lv_event_get_user_data(event), false);
    on_export(event);
}

static void on_back_from_menu(lv_event_t* event) {
    set_overflow_visible((struct Context*)lv_event_get_user_data(event), false);
    on_back(event);
}

/** Updates the "Ln X, Col Y" label from the textarea cursor. */
static void update_position_label(struct Context* context) {
    if (context->editor == NULL || context->position_label == NULL) {
        return;
    }

    const char* text = lv_textarea_get_text(context->editor);
    const uint32_t cursor = lv_textarea_get_cursor_pos(context->editor);

    uint32_t line = 1;
    uint32_t column = 1;
    for (uint32_t index = 0; index < cursor && text[index] != '\0'; index++) {
        const unsigned char byte = (unsigned char)text[index];
        if (byte == '\n') {
            line++;
            column = 1;
        } else if ((byte & 0xC0) != 0x80) {
            // UTF-8 continuation bytes do not start a new character.
            column++;
        }
    }

    char buffer[32];
    snprintf(buffer, sizeof(buffer), "Ln %u, Col %u", (unsigned)line, (unsigned)column);
    lv_label_set_text(context->position_label, buffer);
}

static void on_editor_cursor(lv_event_t* event) {
    update_position_label((struct Context*)lv_event_get_user_data(event));
}

/**
 * Copies the previous line's indentation after Enter, plus one level after a
 * block opener. The textarea inserts the newline itself; this runs after it.
 */
static void on_editor_changed(lv_event_t* event) {
    struct Context* context = (struct Context*)lv_event_get_user_data(event);
    update_position_label(context);
    if (context->auto_indent_guard || context->editor == NULL) {
        return;
    }
    lv_obj_t* editor = context->editor;
    const char* text = lv_textarea_get_text(editor);
    const uint32_t cursor = lv_textarea_get_cursor_pos(editor);
    if (cursor == 0 || text[cursor - 1] != '\n') {
        return;
    }

    const uint32_t previous_end = cursor - 1;
    uint32_t previous_start = previous_end;
    while (previous_start > 0 && text[previous_start - 1] != '\n') {
        previous_start--;
    }

    char indent[64];
    size_t length = 0;
    for (uint32_t index = previous_start; index < previous_end && length < sizeof(indent) - 1; index++) {
        if (text[index] == ' ' || text[index] == '\t') {
            indent[length++] = text[index];
        } else {
            break;
        }
    }

    // Indent one more level after the end of a block opener. This is deliberately
    // conservative: the editor does not parse Lua, so it only reacts to the final
    // non-space character of the previous line.
    uint32_t last = previous_end;
    while (last > previous_start &&
           (text[last - 1] == ' ' || text[last - 1] == '\t' || text[last - 1] == '\r')) {
        last--;
    }
    if (last > previous_start) {
        const char previous = text[last - 1];
        bool opener = previous == '{' || previous == '(' || previous == '[';
        const uint32_t word_length = last - previous_start;
        if (!opener && word_length >= 4 && memcmp(text + last - 4, "then", 4) == 0) {
            opener = true;
        } else if (!opener && word_length >= 2 && memcmp(text + last - 2, "do", 2) == 0) {
            opener = true;
        } else if (!opener && word_length >= 4 && memcmp(text + last - 4, "else", 4) == 0) {
            opener = true;
        }
        if (opener && length + 4 < sizeof(indent)) {
            for (int index = 0; index < 4; index++) {
                indent[length++] = ' ';
            }
        }
    }

    if (length == 0) {
        return;
    }

    indent[length] = '\0';
    context->auto_indent_guard = true;
    lv_textarea_add_text(editor, indent);
    context->auto_indent_guard = false;
}

static void show_editor(struct Context* context, struct ScriptEntry* script) {
    char* source = read_file(script->path);
    if (source == NULL) {
        set_message(context, "Could not open the script");
        show_browser(context);
        return;
    }

    lv_obj_clean(context->content);
    context->editor = NULL;
    context->status_label = NULL;
    context->position_label = NULL;

    context->editing_example = script->source == SCRIPT_SOURCE_EXAMPLE;
    snprintf(context->current_path, sizeof(context->current_path), "%s", script->path);
    if (context->editing_example) {
        context->example_hash = hash_text(source);
    }

    const lv_coord_t gap = (lv_coord_t)lvgl_theme_lerp(4, 12, lvgl_theme_get_spacing());

    const char* name = file_name(script->path);
    char title_text[APP_MANIFEST_NAME_LENGTH + 16];
    if (context->editing_example) {
        snprintf(title_text, sizeof(title_text), "%s (example)", name);
    } else {
        snprintf(title_text, sizeof(title_text), "%s", name);
    }

    lvgl_toolbar_clear_actions(context->toolbar);
    lvgl_toolbar_set_title(context->toolbar, title_text);
    clear_overflow(context);
    add_toolbar_action(context, LVGL_ICON_SHARED_MORE_VERT, on_overflow);
    lv_obj_t* save_action = add_overflow_button(
        context,
        context->editing_example ? LVGL_ICON_SHARED_SAVE_AS : LVGL_ICON_SHARED_SAVE,
        on_save_from_menu
    );
    context->save_label = lv_obj_get_child(save_action, 0);
    add_overflow_button(context, LVGL_ICON_SHARED_PLAY_ARROW, on_run_from_menu);
    add_overflow_button(context, LVGL_ICON_SHARED_DOWNLOAD, on_export_from_menu);
    add_overflow_button(context, LVGL_ICON_SHARED_KEYBOARD_ARROW_LEFT, on_back_from_menu);
    context->title_label = NULL;

    lv_obj_t* editor = lv_textarea_create(context->content);
    lv_obj_set_width(editor, LV_PCT(100));
    lv_obj_set_flex_grow(editor, 1);
    lv_textarea_set_one_line(editor, false);
    // Kerning costs a second glyph lookup per character (and a kerning table search).
    // The editor copies the theme font so it can lay out code without it.
    static lv_font_t editor_font;
    editor_font = *lvgl_get_text_font(FONT_SIZE_SMALL);
    editor_font.kerning = LV_FONT_KERNING_NONE;
    lv_obj_set_style_text_font(editor, &editor_font, 0);
    // lv_textarea_set_text() puts the cursor at the end, which makes LVGL measure the whole
    // text to find the last line. A 25 KB script took 7.6 s there (and tripped the task
    // watchdog). The editor opens at the top anyway, so set the label directly and leave the
    // cursor at position 0.
    lv_label_set_text(lv_obj_get_child(editor, 0), source);
    // Keep the textarea's cursor and scroll state in sync with the label.
    lv_textarea_set_cursor_pos(editor, 0);
    free(source);

    context->editor = editor;
    lv_obj_add_event_cb(editor, on_editor_changed, LV_EVENT_VALUE_CHANGED, context);
    lv_obj_add_event_cb(editor, on_editor_cursor, LV_EVENT_KEY, context);
    lv_obj_add_event_cb(editor, on_editor_cursor, LV_EVENT_CLICKED, context);
    lv_obj_add_event_cb(editor, on_editor_cursor, LV_EVENT_FOCUSED, context);

    lv_obj_t* status_row = lv_obj_create(context->content);
    lv_obj_set_width(status_row, LV_PCT(100));
    lv_obj_set_height(status_row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(status_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(status_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(status_row, 0, 0);
    lv_obj_set_style_pad_column(status_row, gap, 0);
    lv_obj_set_style_border_width(status_row, 0, 0);
    lv_obj_set_style_bg_opa(status_row, LV_OPA_TRANSP, 0);

    context->status_label = lv_label_create(status_row);
    const bool compact = lv_display_get_horizontal_resolution(lv_display_get_default()) < 480;
    if (context->editing_example) {
        lv_label_set_text(
            context->status_label,
            compact ? "Example - Save Copy to edit" : "Bundled example - Run as-is; Save Copy to keep edits"
        );
    } else {
        lv_label_set_text(context->status_label, "Ready");
    }
    lv_obj_set_style_text_font(context->status_label, lvgl_get_text_font(FONT_SIZE_SMALL), 0);
    lv_obj_set_style_text_opa(context->status_label, LV_OPA_70, 0);
    lv_obj_set_flex_grow(context->status_label, 1);
    lv_label_set_long_mode(context->status_label, LV_LABEL_LONG_MODE_DOTS);

    context->position_label = lv_label_create(status_row);
    lv_label_set_text(context->position_label, "Ln 1, Col 1");
    lv_obj_set_style_text_font(context->position_label, lvgl_get_text_font(FONT_SIZE_SMALL), 0);
    lv_obj_set_style_text_opa(context->position_label, LV_OPA_70, 0);

    update_position_label(context);
}

static void on_entry_clicked(lv_event_t* event) {
    struct ScriptEntry* script = (struct ScriptEntry*)lv_event_get_user_data(event);
    show_editor(script->context, script);
}

static void add_script_row(lv_obj_t* list, struct ScriptEntry* script, const char* tag) {
    lv_obj_t* row = lvgl_list_add_button(list, NULL, NULL);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_event_cb(row, on_entry_clicked, LV_EVENT_CLICKED, script);

    lv_obj_t* label = lv_label_create(row);
    lv_label_set_text(label, script->name);
    lv_obj_set_flex_grow(label, 1);
    lv_obj_set_style_text_font(label, lvgl_get_text_font(FONT_SIZE_DEFAULT), 0);

    lv_obj_t* path = lv_label_create(row);
    lv_label_set_text(path, tag);
    lv_obj_set_style_text_font(path, lvgl_get_text_font(FONT_SIZE_SMALL), 0);
    lv_obj_set_style_text_opa(path, LV_OPA_70, 0);
}

static const char* runtime_title(void) {
    static char title[64];
    static bool initialized = false;
    if (!initialized) {
        initialized = true;
        char version[32];
        if (read_manifest_value("version.name=", version, sizeof(version))) {
            snprintf(title, sizeof(title), "Lua %s", version);
        } else {
            snprintf(title, sizeof(title), "Lua");
        }
    }
    return title;
}

static void rebuild_browser_list(struct Context* context) {
    if (context->list_container == NULL) {
        return;
    }
    lv_obj_clean(context->list_container);

    context->example_count = collect_lua_files(
        context, context->examples_dir, context->examples, MAX_ENTRIES, context->filter, SCRIPT_SOURCE_EXAMPLE
    );
    context->entry_count = collect_lua_files(
        context, context->script_dir, context->entries, MAX_ENTRIES, context->filter, SCRIPT_SOURCE_FOLDER
    );

    if (context->example_count == 0 && context->entry_count == 0) {
        lv_obj_t* empty = lv_label_create(context->list_container);
        lv_label_set_text(
            empty,
            context->filter[0] != '\0'
                ? "No scripts match the filter."
                : "No scripts found.\n\nTap New to create one."
        );
        lv_label_set_long_mode(empty, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_width(empty, LV_PCT(100));
        lv_obj_set_style_text_opa(empty, LV_OPA_70, 0);
        return;
    }

    lv_obj_t* card = lvgl_card_create(context->list_container);
    lv_obj_set_width(card, LV_PCT(100));
    lv_obj_set_flex_grow(card, 1);

    lv_obj_t* list = lvgl_list_create(card);
    lv_obj_set_width(list, LV_PCT(100));
    // The card scrolls; the list grows with its rows so only one scrollbar appears.
    lv_obj_set_height(list, LV_SIZE_CONTENT);
    lv_obj_set_scrollable(list, false);

    if (context->example_count > 0) {
        lvgl_list_add_text(list, "Examples");
        for (size_t i = 0; i < context->example_count; i++) {
            add_script_row(list, &context->examples[i], "example");
        }
    }

    if (context->entry_count > 0) {
        lvgl_list_add_text(list, "Scripts");
        for (size_t i = 0; i < context->entry_count; i++) {
            add_script_row(list, &context->entries[i], "lua");
        }
    }

    lvgl_list_hide_last_divider(list);
}

static void on_query_changed(lv_event_t* event) {
    struct Context* context = (struct Context*)lv_event_get_user_data(event);
    if (context->suppress_query) {
        return;
    }

    const char* text = lv_textarea_get_text(context->search_textarea);
    size_t length = strlen(text);
    if (length > MAX_FILTER_LENGTH) {
        length = MAX_FILTER_LENGTH;
    }
    memcpy(context->filter, text, length);
    context->filter[length] = '\0';
    rebuild_browser_list(context);
}

static void on_search_toggle(lv_event_t* event) {
    struct Context* context = (struct Context*)lv_event_get_user_data(event);
    context->search_visible = !context->search_visible;
    if (context->search_row != NULL) {
        lv_obj_set_hidden(context->search_row, !context->search_visible);
    }

    if (context->search_visible) {
        if (context->search_textarea != NULL) {
            lv_group_focus_obj(context->search_textarea);
        }
        return;
    }

    if (context->filter[0] != '\0') {
        context->suppress_query = true;
        if (context->search_textarea != NULL) {
            lv_textarea_set_text(context->search_textarea, "");
        }
        context->suppress_query = false;
        context->filter[0] = '\0';
        rebuild_browser_list(context);
    }
}

static void show_browser(struct Context* context) {
    lv_obj_clean(context->content);
    context->editor = NULL;
    context->status_label = NULL;
    context->position_label = NULL;
    context->title_label = NULL;
    context->save_label = NULL;
    context->search_row = NULL;
    context->search_textarea = NULL;
    context->list_container = NULL;
    context->current_path[0] = '\0';
    context->editing_example = false;

    lvgl_toolbar_clear_actions(context->toolbar);
    lvgl_toolbar_set_title(context->toolbar, runtime_title());
    add_toolbar_action(context, LVGL_ICON_SHARED_SEARCH, on_search_toggle);
    add_toolbar_action(context, LVGL_ICON_SHARED_MORE_VERT, on_overflow);
    clear_overflow(context);
    add_overflow_button(context, LVGL_ICON_SHARED_FOLDER, on_folder_from_menu);
    add_overflow_button(context, LVGL_ICON_SHARED_ADD, on_new_from_menu);

    const lv_coord_t gap = (lv_coord_t)lvgl_theme_lerp(4, 12, lvgl_theme_get_spacing());

    lv_obj_t* caption_row = lv_obj_create(context->content);
    lv_obj_set_width(caption_row, LV_PCT(100));
    lv_obj_set_height(caption_row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(caption_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(caption_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(caption_row, 0, 0);
    lv_obj_set_style_pad_column(caption_row, gap, 0);
    lv_obj_set_style_border_width(caption_row, 0, 0);
    lv_obj_set_style_bg_opa(caption_row, LV_OPA_TRANSP, 0);

    lv_obj_t* caption = lv_label_create(caption_row);
    lv_label_set_text(caption, context->script_dir);
    lv_obj_set_style_text_font(caption, lvgl_get_text_font(FONT_SIZE_SMALL), 0);
    lv_obj_set_style_text_opa(caption, LV_OPA_70, 0);
    lv_label_set_long_mode(caption, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_flex_grow(caption, 1);

    if (context->message[0] != '\0') {
        lv_obj_t* message = lv_label_create(context->content);
        lv_label_set_text(message, context->message);
        lv_obj_set_style_text_font(message, lvgl_get_text_font(FONT_SIZE_SMALL), 0);
        lv_obj_set_style_text_opa(message, LV_OPA_70, 0);
        lv_obj_set_width(message, LV_PCT(100));
        lv_label_set_long_mode(message, LV_LABEL_LONG_MODE_WRAP);
        context->message[0] = '\0';
    }

    lv_obj_t* search_row = lv_obj_create(context->content);
    lv_obj_remove_style_all(search_row);
    lv_obj_set_width(search_row, LV_PCT(100));
    lv_obj_set_height(search_row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(search_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(search_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(search_row, gap, 0);
    lv_obj_set_hidden(search_row, !context->search_visible);
    context->search_row = search_row;

    lv_obj_t* search_icon = lv_label_create(search_row);
    lv_label_set_text(search_icon, LVGL_ICON_SHARED_SEARCH);
    lv_obj_set_style_text_font(search_icon, lvgl_get_shared_icon_default_font(), 0);
    lv_obj_set_style_text_opa(search_icon, LV_OPA_70, 0);

    context->search_textarea = lv_textarea_create(search_row);
    lv_textarea_set_one_line(context->search_textarea, true);
    lv_textarea_set_placeholder_text(context->search_textarea, "Search scripts");
    lv_obj_set_flex_grow(context->search_textarea, 1);
    lv_textarea_set_text(context->search_textarea, context->filter);
    lv_obj_add_event_cb(context->search_textarea, on_query_changed, LV_EVENT_VALUE_CHANGED, context);

    lv_obj_t* list_container = lv_obj_create(context->content);
    lv_obj_set_width(list_container, LV_PCT(100));
    lv_obj_set_flex_grow(list_container, 1);
    lv_obj_set_flex_flow(list_container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(list_container, 0, 0);
    lv_obj_set_style_border_width(list_container, 0, 0);
    lv_obj_set_style_bg_opa(list_container, LV_OPA_TRANSP, 0);
    context->list_container = list_container;

    rebuild_browser_list(context);
}

static void on_close(lv_event_t* event) {
    struct Context* context = (struct Context*)lv_event_get_user_data(event);
    app_event_emit_close(context->app_instance_id);
}

static void create_widgets(lv_obj_t* parent, void* user_data) {
    struct Context* context = (struct Context*)user_data;

    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(parent, 0, 0);
    lv_obj_set_style_pad_row(parent, 0, 0);

    lv_obj_t* toolbar = lvgl_toolbar_create(parent, "Lua");
    lvgl_toolbar_set_nav_action(toolbar, LV_SYMBOL_CLOSE, on_close, context);
    context->toolbar = toolbar;

    const lv_coord_t gap = (lv_coord_t)lvgl_theme_lerp(4, 12, lvgl_theme_get_spacing());

    lv_obj_t* content = lv_obj_create(parent);
    lv_obj_set_width(content, LV_PCT(100));
    lv_obj_set_flex_grow(content, 1);
    lv_obj_set_style_pad_all(content, gap, 0);
    lv_obj_set_style_pad_row(content, gap, 0);
    lv_obj_set_style_border_width(content, 0, 0);
    lv_obj_set_style_radius(content, 0, 0);
    lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);

    context->content = content;

    // Floating menu below the toolbar, populated per view (see show_browser()). Created
    // after the content so it is drawn on top: floating only removes it from the layout.
    context->overflow = lvgl_card_create(parent);
    lv_obj_set_floating(context->overflow, true);
    lv_obj_set_size(context->overflow, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(context->overflow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(
        context->overflow,
        LV_FLEX_ALIGN_CENTER,
        LV_FLEX_ALIGN_CENTER,
        LV_FLEX_ALIGN_CENTER
    );
    lv_obj_set_scrollable(context->overflow, false);
    lvgl_obj_add_edge_padding(context->overflow);
    lvgl_grid_navigation_add(context->overflow);
    lv_obj_add_event_cb(context->overflow, on_overflow_key, LV_EVENT_KEY, context);
    lv_obj_set_hidden(context->overflow, true);

    if (context->script_dir[0] == '\0') {
        lv_obj_t* error = lv_label_create(content);
        lv_label_set_text(error, "Data storage is not available.");
        return;
    }

    show_browser(context);
}

int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;

    static struct Context context = { 0 };
    load_script_dir(context.script_dir, sizeof(context.script_dir));
    load_examples_dir(context.examples_dir, sizeof(context.examples_dir));

    AppInstanceId app_instance_id = app_scheduler_current_app_id();

    struct TaskEventGroup event_group = { 0 };
    task_event_group_construct(&event_group);

    context.event_group = &event_group;
    context.app_instance_id = app_instance_id;

    struct AppEventSubscription sub = { 0 };
    check(app_event_subscribe(&sub, &event_group) == ERROR_NONE);

    WindowId window = window_manager_create(app_instance_id, create_widgets, &context);

    bool should_close = false;
    while (!should_close) {
        task_event_group_wait_any(&event_group, NULL, pdMS_TO_TICKS(250));

        if (context.export_done && !context.export_running) {
            context.export_done = false;
            lvgl_lock();
            if (context.status_label != NULL) {
                set_status(&context, context.export_message);
            } else {
                // The name dialog rebuilt this app, so the browser is showing now.
                set_message(&context, context.export_message);
                show_browser(&context);
            }
            lvgl_unlock();
        }

        struct AppEvent event;
        while (app_event_poll(&sub, &event) == ERROR_NONE) {
            if (event.type == APP_EVENT_CLOSE) {
                should_close = true;
                break;
            }

            if (event.type == APP_EVENT_RESULT && context.dialog_launch_id != 0 && event.result.launch_id == context.dialog_launch_id) {
                const uint32_t launch_id = context.dialog_launch_id;
                const enum DialogKind kind = context.dialog_kind;
                context.dialog_launch_id = 0;
                context.dialog_kind = DIALOG_NONE;

                char text[sizeof(context.dialog_buffer)];
                size_t length = 0;
                if (event.result.result == 0) {
                    length = app_stream_read(&context.dialog_stream, text, sizeof(text) - 1);
                    text[length] = '\0';
                    while (length > 0 && (
                        text[length - 1] == '\n' || text[length - 1] == '\r' || text[length - 1] == ' '
                    )) {
                        text[--length] = '\0';
                    }
                }

                app_stream_unsubscribe(&context.dialog_stream);

                if (kind == DIALOG_FOLDER) {
                    while (length > 1 && text[length - 1] == '/') {
                        text[--length] = '\0';
                    }
                    if (length > 0) {
                        snprintf(context.script_dir, sizeof(context.script_dir), "%s", text);
                        save_script_dir(context.script_dir);
                        lvgl_lock();
                        show_browser(&context);
                        lvgl_unlock();
                    }
                } else if (kind == DIALOG_NEW_FILE && length > 0) {
                    lvgl_lock();
                    create_new_script(&context, text);
                    lvgl_unlock();
                } else if (kind == DIALOG_EXPORT_NAME && length > 0) {
                    lvgl_lock();
                    start_export(&context, text);
                    lvgl_unlock();
                }

                app_manager_stop(launch_id);
            }
        }
    }

    if (context.dialog_launch_id != 0) {
        app_stream_unsubscribe(&context.dialog_stream);
        app_manager_stop(context.dialog_launch_id);
    }

    window_manager_remove(window);
    check(app_event_unsubscribe(&sub) == ERROR_NONE);
    task_event_group_destruct(&event_group);

    return 0;
}
