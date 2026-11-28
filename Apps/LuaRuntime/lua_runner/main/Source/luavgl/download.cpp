// SPDX-License-Identifier: Apache-2.0
#include <luavgl/object_private.h>

#include <lua/runtime.h>

#include <http/client.h>

#include <tactility/error.h>
#include <tactility/log.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>

#include <sys/stat.h>

#ifdef ESP_PLATFORM
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#else
#include <thread>
#endif

extern "C" {
#include <lauxlib.h>
}

/**
 * `tactility.http.download(options, callback)`
 *
 * Streams a GET response to a file on a worker task. The firmware's `http_client`
 * does the request, so this works on the device and in the simulator, verifies the
 * platform's roots or a supplied certificate, and can be cancelled mid-transfer.
 *
 * A separate worker rather than `http_download_start()`: this exposes the status,
 * content length and progress, and follows redirects itself (the client doesn't).
 * The worker only touches C data; the runner's app loop delivers the Lua callbacks
 * with the LVGL lock held.
 */

namespace {

constexpr auto* TAG = "luavgl.download";
constexpr auto* DOWNLOAD_METATABLE = "tactility.http.Download";

constexpr int MAX_CONCURRENT = 2;
constexpr int MAX_HEADERS = 8;
constexpr int DEFAULT_TIMEOUT_MS = 15000;
constexpr int HARD_TIMEOUT_MS = 60000;
constexpr int MAX_REDIRECTS = 5;
constexpr size_t BUFFER_SIZE = 4096;

#ifdef ESP_PLATFORM
constexpr uint32_t WORKER_STACK_BYTES = 16384;
constexpr UBaseType_t WORKER_PRIORITY = 5;
#endif

/** Registry key for live downloads, so shutdown can abandon them safely. */
char DOWNLOADS_KEY = 0;

struct HttpHeader {
    char* name = nullptr;
    char* value = nullptr;
};

struct DownloadRequest {
    lua_State* state = nullptr;
    int callback_ref = LUA_NOREF;
    int progress_ref = LUA_NOREF;
    int pin_ref = LUA_NOREF;

    char* url = nullptr;
    char* path = nullptr;
    char* certificate = nullptr;
    HttpHeader headers[MAX_HEADERS];
    int header_count = 0;
    int timeout_ms = DEFAULT_TIMEOUT_MS;

    std::atomic<bool> cancel_requested{false};
    std::atomic<bool> done{false};
    std::atomic<HttpClient*> client{nullptr};
    std::atomic<uint32_t> received{0};
    std::atomic<uint32_t> total{0};

    int status = 0;
    bool cancelled = false;
    char error[128] = {};

    bool finished = false;
    bool abandoned = false;

    uint32_t last_progress = 0;
};

struct DownloadHandle {
    DownloadRequest* request;
};

int push_failure(lua_State* state, const char* message) {
    lua_pushnil(state);
    lua_pushstring(state, message);
    return 2;
}

char* copy_string(const char* text) {
    const size_t length = std::strlen(text);
    auto* copy = static_cast<char*>(std::malloc(length + 1));
    if (copy != nullptr) {
        std::memcpy(copy, text, length + 1);
    }
    return copy;
}

void free_request(DownloadRequest* request) {
    if (request->client.load() != nullptr) {
        http_client_free(request->client.exchange(nullptr));
    }
    std::free(request->url);
    std::free(request->path);
    std::free(request->certificate);
    for (int index = 0; index < request->header_count; index++) {
        std::free(request->headers[index].name);
        std::free(request->headers[index].value);
    }
    delete request;
}

void push_downloads(lua_State* state) {
    lua_rawgetp(state, LUA_REGISTRYINDEX, &DOWNLOADS_KEY);
    if (lua_isnil(state, -1)) {
        lua_pop(state, 1);
        lua_newtable(state);
        lua_pushvalue(state, -1);
        lua_rawsetp(state, LUA_REGISTRYINDEX, &DOWNLOADS_KEY);
    }
}

void remove_download(lua_State* state, DownloadRequest* request) {
    push_downloads(state);
    lua_pushlightuserdata(state, request);
    lua_pushnil(state);
    lua_rawset(state, -3);
    lua_pop(state, 1);
}

int count_downloads(lua_State* state) {
    push_downloads(state);
    int count = 0;
    lua_pushnil(state);
    while (lua_next(state, -2) != 0) {
        count++;
        lua_pop(state, 1);
    }
    lua_pop(state, 1);
    return count;
}

void release_request(DownloadRequest* request) {
    if (request->finished) {
        return;
    }
    request->finished = true;

    lua_State* state = request->state;
    if (state == nullptr) {
        return;
    }

    if (request->callback_ref != LUA_NOREF) {
        luaL_unref(state, LUA_REGISTRYINDEX, request->callback_ref);
        request->callback_ref = LUA_NOREF;
    }
    if (request->progress_ref != LUA_NOREF) {
        luaL_unref(state, LUA_REGISTRYINDEX, request->progress_ref);
        request->progress_ref = LUA_NOREF;
    }
    if (request->pin_ref != LUA_NOREF) {
        luaL_unref(state, LUA_REGISTRYINDEX, request->pin_ref);
        request->pin_ref = LUA_NOREF;
    }

    remove_download(state, request);
}

void abandon_request(DownloadRequest* request) {
    if (request->finished) {
        return;
    }
    request->finished = true;
    request->abandoned = true;
    request->cancel_requested.store(true, std::memory_order_relaxed);
}

void set_error(DownloadRequest* request, const char* message) {
    if (request->error[0] == '\0') {
        std::snprintf(request->error, sizeof(request->error), "%s", message);
    }
}

const char* failure_message(HttpClientFailure failure) {
    switch (failure) {
        case HTTP_CLIENT_FAILURE_URL: return "invalid URL";
        case HTTP_CLIENT_FAILURE_CONNECT: return "could not connect";
        case HTTP_CLIENT_FAILURE_CERTIFICATE: return "certificate verification failed";
        case HTTP_CLIENT_FAILURE_PROTOCOL: return "invalid response";
        case HTTP_CLIENT_FAILURE_CANCELLED: return "cancelled";
        case HTTP_CLIENT_FAILURE_MEMORY: return "out of memory";
        case HTTP_CLIENT_FAILURE_NOT_OPEN: return "no open response";
        default: return "request failed";
    }
}

std::string resolve_location(const std::string& base, const std::string& location) {
    if (location.rfind("http://", 0) == 0 || location.rfind("https://", 0) == 0) {
        return location;
    }

    const size_t scheme = base.find("://");
    if (scheme == std::string::npos) {
        return location;
    }
    if (location.rfind("//", 0) == 0) {
        return base.substr(0, scheme) + ":" + location;
    }

    const size_t host_start = scheme + 3;
    const size_t path_start = base.find('/', host_start);
    const std::string origin = base.substr(0, path_start == std::string::npos ? base.size() : path_start);
    if (!location.empty() && location[0] == '/') {
        return origin + location;
    }

    const size_t slash = base.rfind('/');
    if (slash == std::string::npos || slash < host_start) {
        return origin + "/" + location;
    }
    return base.substr(0, slash + 1) + location;
}

void worker(void* argument) {
    auto* request = static_cast<DownloadRequest*>(argument);

    HttpClient* client = http_client_alloc();
    request->client.store(client);
    if (client == nullptr) {
        set_error(request, "out of memory");
        request->done.store(true, std::memory_order_release);
#ifdef ESP_PLATFORM
        vTaskDelete(nullptr);
#endif
        return;
    }

    http_client_set_timeout(client, static_cast<uint32_t>(request->timeout_ms));
    if (request->certificate != nullptr && request->certificate[0] != '\0') {
        http_client_set_certificate(client, request->certificate);
    }
    for (int index = 0; index < request->header_count; index++) {
        if (http_client_set_header(client, request->headers[index].name, request->headers[index].value) != ERROR_NONE) {
            set_error(request, "invalid request header");
            break;
        }
    }

    std::string url = request->url;
    bool opened = false;

    if (request->error[0] == '\0') {
        for (int redirect = 0; redirect <= MAX_REDIRECTS; redirect++) {
            if (request->cancel_requested.load(std::memory_order_relaxed)) {
                request->cancelled = true;
                break;
            }

            const error_t error = http_client_open(client, url.c_str());
            if (error != ERROR_NONE) {
                if (request->cancel_requested.load(std::memory_order_relaxed)) {
                    request->cancelled = true;
                } else {
                    set_error(request, failure_message(http_client_get_failure(client)));
                }
                break;
            }

            const int status = http_client_get_status_code(client);
            if (status >= 300 && status < 400) {
                const char* location = http_client_get_header(client, "location");
                if (location == nullptr || location[0] == '\0') {
                    set_error(request, "redirect without a location");
                    break;
                }
                url = resolve_location(url, location);
                http_client_close(client);
                continue;
            }

            request->status = status;
            if (status < 200 || status >= 300) {
                char message[64];
                std::snprintf(message, sizeof(message), "server returned %d", status);
                set_error(request, message);
                break;
            }

            const int64_t content_length = http_client_get_content_length(client);
            if (content_length > 0) {
                request->total.store(
                    content_length > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(content_length),
                    std::memory_order_relaxed
                );
            }

            FILE* file = std::fopen(request->path, "wb");
            if (file == nullptr) {
                set_error(request, "could not open the target file");
                break;
            }

            uint32_t written = 0;
            bool ok = true;
            char buffer[BUFFER_SIZE];

            while (true) {
                if (request->cancel_requested.load(std::memory_order_relaxed)) {
                    http_client_cancel(client);
                    request->cancelled = true;
                    ok = false;
                    break;
                }

                const int read = http_client_read(client, buffer, sizeof(buffer));
                if (read < 0) {
                    if (request->cancel_requested.load(std::memory_order_relaxed)) {
                        request->cancelled = true;
                    } else {
                        set_error(request, "failed to read the response");
                    }
                    ok = false;
                    break;
                }
                if (read == 0) {
                    break;
                }

                if (std::fwrite(buffer, 1, static_cast<size_t>(read), file) != static_cast<size_t>(read)) {
                    set_error(request, "failed to write the target file");
                    ok = false;
                    break;
                }

                written += static_cast<uint32_t>(read);
                request->received.store(written, std::memory_order_relaxed);
            }

            if (std::fclose(file) != 0 && ok) {
                set_error(request, "failed to finalize the target file");
                ok = false;
            }
            if (!ok) {
                std::remove(request->path);
            }

            opened = true;
            break;
        }
    }

    if (!opened && request->error[0] == '\0' && !request->cancelled) {
        set_error(request, "download failed");
    }

    http_client_close(client);
    request->done.store(true, std::memory_order_release);

#ifdef ESP_PLATFORM
    vTaskDelete(nullptr);
#endif
}

/** `tactility.http.download(options, callback) -> handle | nil, reason` */
int http_download(lua_State* state) {
    luaL_checktype(state, 1, LUA_TTABLE);
    luaL_checktype(state, 2, LUA_TFUNCTION);

    if (count_downloads(state) >= MAX_CONCURRENT) {
        return push_failure(state, "too many active downloads");
    }

    auto* request = new (std::nothrow) DownloadRequest();
    if (request == nullptr) {
        return push_failure(state, "out of memory");
    }
    request->state = state;

    lua_getfield(state, 1, "url");
    if (!lua_isstring(state, -1)) {
        lua_pop(state, 1);
        free_request(request);
        return push_failure(state, "url is required");
    }
    request->url = copy_string(lua_tostring(state, -1));
    lua_pop(state, 1);

    lua_getfield(state, 1, "path");
    if (!lua_isstring(state, -1)) {
        lua_pop(state, 1);
        free_request(request);
        return push_failure(state, "path is required");
    }
    request->path = copy_string(lua_tostring(state, -1));
    lua_pop(state, 1);

    lua_getfield(state, 1, "certificate");
    if (lua_isstring(state, -1)) {
        request->certificate = copy_string(lua_tostring(state, -1));
        if (request->certificate == nullptr) {
            lua_pop(state, 1);
            free_request(request);
            return push_failure(state, "out of memory");
        }
    }
    lua_pop(state, 1);

    lua_getfield(state, 1, "timeout_ms");
    if (lua_isinteger(state, -1)) {
        const lua_Integer timeout = lua_tointeger(state, -1);
        request->timeout_ms = static_cast<int>(
            timeout < 1000 ? 1000 : (timeout > HARD_TIMEOUT_MS ? HARD_TIMEOUT_MS : timeout)
        );
    }
    lua_pop(state, 1);

    lua_getfield(state, 1, "headers");
    if (lua_istable(state, -1)) {
        lua_pushnil(state);
        while (lua_next(state, -2) != 0) {
            if (lua_type(state, -2) != LUA_TSTRING || !lua_isstring(state, -1) ||
                request->header_count >= MAX_HEADERS) {
                lua_pop(state, 2);
                lua_pop(state, 1);
                free_request(request);
                return push_failure(state, "headers must be up to 8 string pairs");
            }
            HttpHeader& header = request->headers[request->header_count];
            header.name = copy_string(lua_tostring(state, -2));
            header.value = copy_string(lua_tostring(state, -1));
            if (header.name == nullptr || header.value == nullptr) {
                lua_pop(state, 2);
                lua_pop(state, 1);
                free_request(request);
                return push_failure(state, "out of memory");
            }
            request->header_count++;
            lua_pop(state, 1);
        }
    }
    lua_pop(state, 1);

    lua_getfield(state, 1, "progress");
    if (lua_isfunction(state, -1)) {
        request->progress_ref = luaL_ref(state, LUA_REGISTRYINDEX);
    } else if (!lua_isnil(state, -1)) {
        lua_pop(state, 1);
        free_request(request);
        return push_failure(state, "progress must be a function");
    } else {
        lua_pop(state, 1);
    }

    if (request->url == nullptr || request->path == nullptr) {
        free_request(request);
        return push_failure(state, "out of memory");
    }

    lua_pushvalue(state, 2);
    request->callback_ref = luaL_ref(state, LUA_REGISTRYINDEX);

    auto* handle = static_cast<DownloadHandle*>(lua_newuserdatauv(state, sizeof(DownloadHandle), 0));
    handle->request = request;
    luaL_setmetatable(state, DOWNLOAD_METATABLE);

    lua_pushvalue(state, -1);
    request->pin_ref = luaL_ref(state, LUA_REGISTRYINDEX);

    push_downloads(state);
    lua_pushlightuserdata(state, request);
    lua_pushvalue(state, -3);
    lua_rawset(state, -3);
    lua_pop(state, 1);

#ifdef ESP_PLATFORM
    if (xTaskCreate(worker, "lua_download", WORKER_STACK_BYTES, request, WORKER_PRIORITY, nullptr) != pdPASS) {
        release_request(request);
        return push_failure(state, "could not start the download task");
    }
#else
    try {
        std::thread(worker, request).detach();
    } catch (...) {
        release_request(request);
        return push_failure(state, "could not start the download thread");
    }
#endif

    return 1;
}

int download_cancel(lua_State* state) {
    auto* handle = static_cast<DownloadHandle*>(luaL_checkudata(state, 1, DOWNLOAD_METATABLE));
    if (handle->request != nullptr && !handle->request->finished) {
        handle->request->cancel_requested.store(true, std::memory_order_relaxed);
        // The worker calls http_client_cancel() itself when it sees this flag. Calling
        // it here races with a worker that is inside http_client_open() or cleanup.
    }
    lua_pushboolean(state, 1);
    return 1;
}

int download_is_active(lua_State* state) {
    auto* handle = static_cast<DownloadHandle*>(luaL_checkudata(state, 1, DOWNLOAD_METATABLE));
    lua_pushboolean(state, handle->request != nullptr && !handle->request->finished);
    return 1;
}

int download_gc(lua_State* state) {
    auto* handle = static_cast<DownloadHandle*>(luaL_checkudata(state, 1, DOWNLOAD_METATABLE));
    if (handle->request != nullptr) {
        if (!handle->request->abandoned) {
            release_request(handle->request);
            free_request(handle->request);
        }
        handle->request = nullptr;
    }
    return 0;
}

const luaL_Reg download_methods[] = {
    { "cancel", download_cancel },
    { "is_active", download_is_active },
    { nullptr, nullptr }
};

const luaL_Reg download_functions[] = {
    { "download", http_download },
    { nullptr, nullptr }
};

void deliver_result(DownloadRequest* request) {
    lua_State* state = request->state;
    lua_rawgeti(state, LUA_REGISTRYINDEX, request->callback_ref);
    if (lua_type(state, -1) != LUA_TFUNCTION) {
        lua_pop(state, 1);
        release_request(request);
        return;
    }

    if (request->cancelled) {
        lua_pushnil(state);
        lua_pushstring(state, "cancelled");
        if (lua_pcall(state, 2, 0, 0) != LUA_OK) {
            LOG_E(TAG, "download callback: %s", lua_tostring(state, -1));
            lua_pop(state, 1);
        }
    } else if (request->error[0] != '\0') {
        lua_pushnil(state);
        lua_pushstring(state, request->error);
        if (lua_pcall(state, 2, 0, 0) != LUA_OK) {
            LOG_E(TAG, "download callback: %s", lua_tostring(state, -1));
            lua_pop(state, 1);
        }
    } else {
        lua_createtable(state, 0, 3);
        lua_pushinteger(state, request->status);
        lua_setfield(state, -2, "status");
        lua_pushinteger(state, static_cast<lua_Integer>(request->received.load(std::memory_order_relaxed)));
        lua_setfield(state, -2, "length");
        lua_pushstring(state, request->path);
        lua_setfield(state, -2, "path");
        if (lua_pcall(state, 1, 0, 0) != LUA_OK) {
            LOG_E(TAG, "download callback: %s", lua_tostring(state, -1));
            lua_pop(state, 1);
        }
    }

    release_request(request);
}

void deliver_progress(DownloadRequest* request) {
    if (request->progress_ref == LUA_NOREF) {
        return;
    }

    const uint32_t received = request->received.load(std::memory_order_relaxed);
    if (received == request->last_progress) {
        return;
    }
    request->last_progress = received;

    lua_State* state = request->state;
    lua_rawgeti(state, LUA_REGISTRYINDEX, request->progress_ref);
    if (lua_type(state, -1) != LUA_TFUNCTION) {
        lua_pop(state, 1);
        return;
    }

    lua_pushinteger(state, static_cast<lua_Integer>(received));
    lua_pushinteger(state, static_cast<lua_Integer>(request->total.load(std::memory_order_relaxed)));
    if (lua_pcall(state, 2, 0, 0) != LUA_OK) {
        LOG_E(TAG, "download progress: %s", lua_tostring(state, -1));
        lua_pop(state, 1);
    }
}

} // namespace

void luavgl_open_downloads(lua_State* state) {
    luaL_newmetatable(state, DOWNLOAD_METATABLE);
    luaL_newlib(state, download_methods);
    lua_setfield(state, -2, "__index");
    lua_pushcfunction(state, download_gc);
    lua_setfield(state, -2, "__gc");
    lua_pushboolean(state, 0);
    lua_setfield(state, -2, "__metatable");
    lua_pop(state, 1);

    lua_getglobal(state, "tactility");
    if (!lua_istable(state, -1)) {
        lua_pop(state, 1);
        LOG_E(TAG, "tactility table is missing");
        return;
    }
    lua_getfield(state, -1, "http");
    if (!lua_istable(state, -1)) {
        lua_pop(state, 2);
        LOG_E(TAG, "tactility.http table is missing");
        return;
    }
    luaL_setfuncs(state, download_functions, 0);
    lua_pop(state, 2);
}

extern "C" void luavgl_downloads_poll(struct LuaRuntime* runtime) {
    auto* state = static_cast<lua_State*>(lua_runtime_get_state(runtime));
    push_downloads(state);

    DownloadRequest* requests[MAX_CONCURRENT + 4];
    size_t count = 0;

    lua_pushnil(state);
    while (lua_next(state, -2) != 0) {
        lua_pop(state, 1);
        if (count < sizeof(requests) / sizeof(requests[0])) {
            requests[count++] = static_cast<DownloadRequest*>(lua_touserdata(state, -1));
        }
    }
    lua_pop(state, 1);

    for (size_t index = 0; index < count; index++) {
        DownloadRequest* request = requests[index];
        if (request == nullptr || request->finished) {
            continue;
        }
        if (request->done.load(std::memory_order_acquire)) {
            deliver_result(request);
        } else {
            deliver_progress(request);
        }
    }
}

void luavgl_delete_all_downloads(lua_State* state) {
    push_downloads(state);

    DownloadRequest* requests[MAX_CONCURRENT + 4];
    size_t count = 0;
    lua_pushnil(state);
    while (lua_next(state, -2) != 0) {
        lua_pop(state, 1);
        if (count < sizeof(requests) / sizeof(requests[0])) {
            requests[count++] = static_cast<DownloadRequest*>(lua_touserdata(state, -1));
        }
    }
    lua_pop(state, 1);

    for (size_t index = 0; index < count; index++) {
        abandon_request(requests[index]);
    }
}
