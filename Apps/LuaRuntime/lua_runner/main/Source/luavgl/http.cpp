// SPDX-License-Identifier: Apache-2.0
#include <luavgl/object_private.h>

#include <lua/runtime.h>

#include <tactility/log.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" {
#include <lauxlib.h>
}

#ifdef ESP_PLATFORM
#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#endif

/**
 * `tactility.http.request(options, callback)`
 *
 * The request runs on its own task, because scripts are evaluated with the LVGL
 * lock held: a blocking request there would freeze the display. The result is
 * handed to the callback by the runner's app loop, with the LVGL lock held, so a
 * callback can update widgets.
 *
 * This is an ESP32 feature. The simulator has no TLS stack or HTTP client, so the
 * binding exists there but request() returns an error.
 */

namespace {

constexpr auto* TAG = "luavgl.http";
constexpr auto* HTTP_METATABLE = "tactility.http.Request";

constexpr int MAX_CONCURRENT = 4;
constexpr int MAX_HEADERS = 8;
constexpr size_t MAX_BODY_BYTES = 64 * 1024;
constexpr size_t DEFAULT_MAX_RESPONSE = 128 * 1024;
constexpr size_t HARD_MAX_RESPONSE = 1024 * 1024;
constexpr int DEFAULT_TIMEOUT_MS = 10000;
constexpr int HARD_TIMEOUT_MS = 60000;
constexpr size_t CONTENT_TYPE_LENGTH = 96;

#ifdef ESP_PLATFORM
constexpr uint32_t WORKER_STACK_BYTES = 12288;
constexpr UBaseType_t WORKER_PRIORITY = 5;
#endif

/** Registry key for live requests, so shutdown can abandon them safely. */
char REQUESTS_KEY = 0;

struct HttpHeader {
    char* name;
    char* value;
};

struct HttpRequest {
    lua_State* state;
    int callback_ref;
    int pin_ref;

    char* url;
    char* method;
    char* request_body;
    size_t request_body_length;
    HttpHeader headers[MAX_HEADERS];
    int header_count;
    int timeout_ms;
    size_t max_response;

    std::atomic<bool> done;
    std::atomic<bool> cancel_requested;

    bool cancelled;
    int status;
    char* response_body;
    size_t response_length;
    char content_type[CONTENT_TYPE_LENGTH];
    char error[128];

    bool finished;
    bool abandoned;
};

struct HttpHandle {
    HttpRequest* request;
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

void free_request(HttpRequest* request) {
    std::free(request->url);
    std::free(request->method);
    std::free(request->request_body);
    for (int i = 0; i < request->header_count; i++) {
        std::free(request->headers[i].name);
        std::free(request->headers[i].value);
    }
    std::free(request->response_body);
    std::free(request);
}

void push_requests(lua_State* state) {
    lua_rawgetp(state, LUA_REGISTRYINDEX, &REQUESTS_KEY);
    if (lua_isnil(state, -1)) {
        lua_pop(state, 1);
        lua_newtable(state);
        lua_pushvalue(state, -1);
        lua_rawsetp(state, LUA_REGISTRYINDEX, &REQUESTS_KEY);
    }
}

void remove_request(lua_State* state, HttpRequest* request) {
    push_requests(state);
    lua_pushlightuserdata(state, request);
    lua_pushnil(state);
    lua_rawset(state, -3);
    lua_pop(state, 1);
}

int count_requests(lua_State* state) {
    push_requests(state);
    int count = 0;
    lua_pushnil(state);
    while (lua_next(state, -2) != 0) {
        count++;
        lua_pop(state, 1);
    }
    lua_pop(state, 1);
    return count;
}

/** Releases the Lua-side references; the handle's finalizer frees the struct. */
void release_request(HttpRequest* request) {
    if (request->finished) {
        return;
    }
    request->finished = true;

    lua_State* state = request->state;
    if (state != nullptr) {
        if (request->callback_ref != LUA_NOREF) {
            luaL_unref(state, LUA_REGISTRYINDEX, request->callback_ref);
            request->callback_ref = LUA_NOREF;
        }
        if (request->pin_ref != LUA_NOREF) {
            luaL_unref(state, LUA_REGISTRYINDEX, request->pin_ref);
            request->pin_ref = LUA_NOREF;
        }
        remove_request(state, request);
    }

    std::free(request->response_body);
    request->response_body = nullptr;
    request->response_length = 0;
}

void abandon_request(HttpRequest* request) {
    // The worker may still be writing response fields. Do not free it: leaking a
    // finished request on app close is safer than a use-after-free.
    request->abandoned = true;
}

#ifdef ESP_PLATFORM
esp_http_client_method_t method_from_string(const char* method) {
    if (std::strcmp(method, "POST") == 0) return HTTP_METHOD_POST;
    if (std::strcmp(method, "PUT") == 0) return HTTP_METHOD_PUT;
    if (std::strcmp(method, "PATCH") == 0) return HTTP_METHOD_PATCH;
    if (std::strcmp(method, "DELETE") == 0) return HTTP_METHOD_DELETE;
    if (std::strcmp(method, "HEAD") == 0) return HTTP_METHOD_HEAD;
    return HTTP_METHOD_GET;
}

void set_error(HttpRequest* request, const char* message) {
    if (request->error[0] == '\0') {
        std::snprintf(request->error, sizeof(request->error), "%s", message);
    }
}

void worker(void* argument) {
    auto* request = static_cast<HttpRequest*>(argument);

    esp_http_client_config_t config = {};
    config.url = request->url;
    config.timeout_ms = request->timeout_ms;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.max_redirection_count = 5;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        set_error(request, "could not create the HTTP client");
        request->done.store(true, std::memory_order_release);
        vTaskDelete(nullptr);
        return;
    }

    esp_http_client_set_method(client, method_from_string(request->method));
    for (int i = 0; i < request->header_count; i++) {
        esp_http_client_set_header(client, request->headers[i].name, request->headers[i].value);
    }

    const bool sends_body = request->request_body_length > 0 &&
        std::strcmp(request->method, "GET") != 0 &&
        std::strcmp(request->method, "HEAD") != 0;

    if (esp_http_client_open(client, sends_body ? static_cast<int>(request->request_body_length) : 0) != ESP_OK) {
        set_error(request, "could not open the connection");
    } else {
        if (sends_body) {
            const int written = esp_http_client_write(
                client,
                request->request_body,
                static_cast<int>(request->request_body_length)
            );
            if (written < 0 || static_cast<size_t>(written) != request->request_body_length) {
                set_error(request, "could not send the request body");
            }
        }

        if (request->error[0] == '\0' && esp_http_client_fetch_headers(client) < 0) {
            set_error(request, "could not read the response headers");
        }

        if (request->error[0] == '\0') {
            request->status = esp_http_client_get_status_code(client);
            const int64_t content_length = esp_http_client_get_content_length(client);
            if (content_length > 0 && static_cast<size_t>(content_length) > request->max_response) {
                set_error(request, "response is larger than max_bytes");
            }

            if (request->error[0] == '\0') {
                char* content_type = nullptr;
                if (esp_http_client_get_header(client, "Content-Type", &content_type) == ESP_OK &&
                    content_type != nullptr) {
                    std::snprintf(
                        request->content_type,
                        sizeof(request->content_type),
                        "%s",
                        content_type
                    );
                }

                size_t capacity = 4096;
                if (content_length > 0) {
                    capacity = static_cast<size_t>(content_length);
                }
                if (capacity > request->max_response) {
                    capacity = request->max_response;
                }
                if (capacity < 256) {
                    capacity = 256;
                }

                char* body = static_cast<char*>(std::malloc(capacity + 1));
                size_t length = 0;
                if (body == nullptr) {
                    set_error(request, "out of memory");
                } else {
                    while (request->error[0] == '\0') {
                        if (request->cancel_requested.load(std::memory_order_relaxed)) {
                            request->cancelled = true;
                            break;
                        }

                        char chunk[1024];
                        const int read = esp_http_client_read(client, chunk, sizeof(chunk));
                        if (read < 0) {
                            set_error(request, "could not read the response body");
                            break;
                        }
                        if (read == 0) {
                            break;
                        }

                        if (length + static_cast<size_t>(read) > request->max_response) {
                            set_error(request, "response is larger than max_bytes");
                            break;
                        }

                        if (length + static_cast<size_t>(read) > capacity) {
                            size_t next = capacity * 2;
                            if (next > request->max_response) {
                                next = request->max_response;
                            }
                            char* grown = static_cast<char*>(std::realloc(body, next + 1));
                            if (grown == nullptr) {
                                set_error(request, "out of memory");
                                break;
                            }
                            body = grown;
                            capacity = next;
                        }

                        std::memcpy(body + length, chunk, static_cast<size_t>(read));
                        length += static_cast<size_t>(read);
                    }

                    if (request->error[0] == '\0' && !request->cancelled) {
                        body[length] = '\0';
                        request->response_body = body;
                        request->response_length = length;
                    } else {
                        std::free(body);
                    }
                }
            }
        }
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    request->done.store(true, std::memory_order_release);
    vTaskDelete(nullptr);
}
#endif

void push_response(lua_State* state, const HttpRequest* request) {
    lua_createtable(state, 0, 4);
    lua_pushinteger(state, request->status);
    lua_setfield(state, -2, "status");
    lua_pushlstring(state, request->response_body != nullptr ? request->response_body : "", request->response_length);
    lua_setfield(state, -2, "body");
    lua_pushinteger(state, static_cast<lua_Integer>(request->response_length));
    lua_setfield(state, -2, "length");
    if (request->content_type[0] != '\0') {
        lua_pushstring(state, request->content_type);
        lua_setfield(state, -2, "content_type");
    }
}

/**
 * `tactility.http.request(options, callback) -> handle | nil, reason`
 *
 * Starts an asynchronous request. Options are `url` (required), `method`,
 * `headers`, `body`, `timeout_ms` and `max_bytes`. The callback receives
 * `{ status, body, length, content_type }`, or `nil, reason` for a transport
 * failure. Up to four requests may run at once.
 */
int http_request(lua_State* state) {
    luaL_checktype(state, 1, LUA_TTABLE);
    luaL_checktype(state, 2, LUA_TFUNCTION);

    // A request only costs PSRAM/stack once the worker starts, but the limit keeps a
    // script from filling both with a loop.
    if (count_requests(state) >= MAX_CONCURRENT) {
        return push_failure(state, "too many active HTTP requests");
    }

    auto* request = static_cast<HttpRequest*>(std::calloc(1, sizeof(HttpRequest)));
    if (request == nullptr) {
        return push_failure(state, "out of memory");
    }
    request->state = state;
    request->callback_ref = LUA_NOREF;
    request->pin_ref = LUA_NOREF;
    request->timeout_ms = DEFAULT_TIMEOUT_MS;
    request->max_response = DEFAULT_MAX_RESPONSE;
    request->cancelled = false;

    lua_getfield(state, 1, "url");
    if (!lua_isstring(state, -1)) {
        lua_pop(state, 1);
        free_request(request);
        return push_failure(state, "url is required");
    }
    request->url = copy_string(lua_tostring(state, -1));
    lua_pop(state, 1);

    lua_getfield(state, 1, "method");
    request->method = lua_isstring(state, -1)
        ? copy_string(lua_tostring(state, -1))
        : copy_string("GET");
    lua_pop(state, 1);

    lua_getfield(state, 1, "body");
    if (lua_isstring(state, -1)) {
        size_t length = 0;
        const char* body = lua_tolstring(state, -1, &length);
        if (length > MAX_BODY_BYTES) {
            lua_pop(state, 1);
            free_request(request);
            return push_failure(state, "request body is too large");
        }
        request->request_body = static_cast<char*>(std::malloc(length + 1));
        if (request->request_body == nullptr) {
            lua_pop(state, 1);
            free_request(request);
            return push_failure(state, "out of memory");
        }
        std::memcpy(request->request_body, body, length);
        request->request_body[length] = '\0';
        request->request_body_length = length;
    }
    lua_pop(state, 1);

    lua_getfield(state, 1, "timeout_ms");
    if (lua_isinteger(state, -1)) {
        const lua_Integer timeout = lua_tointeger(state, -1);
        request->timeout_ms = static_cast<int>(timeout < 1000 ? 1000 : (timeout > HARD_TIMEOUT_MS ? HARD_TIMEOUT_MS : timeout));
    }
    lua_pop(state, 1);

    lua_getfield(state, 1, "max_bytes");
    if (lua_isinteger(state, -1)) {
        const lua_Integer max = lua_tointeger(state, -1);
        request->max_response = static_cast<size_t>(
            max < 1024 ? 1024 : (max > static_cast<lua_Integer>(HARD_MAX_RESPONSE) ? HARD_MAX_RESPONSE : max)
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
            request->header_count++;
            lua_pop(state, 1); // value; key stays for lua_next
        }
    }
    lua_pop(state, 1);

    if (request->url == nullptr || request->method == nullptr) {
        free_request(request);
        return push_failure(state, "out of memory");
    }

    lua_pushvalue(state, 2);
    request->callback_ref = luaL_ref(state, LUA_REGISTRYINDEX);

    auto* handle = static_cast<HttpHandle*>(lua_newuserdatauv(state, sizeof(HttpHandle), 0));
    handle->request = request;
    luaL_setmetatable(state, HTTP_METATABLE);

    lua_pushvalue(state, -1);
    request->pin_ref = luaL_ref(state, LUA_REGISTRYINDEX);

    push_requests(state);
    lua_pushlightuserdata(state, request);
    lua_pushvalue(state, -3);
    lua_rawset(state, -3);
    lua_pop(state, 1);

#ifdef ESP_PLATFORM
    if (xTaskCreate(worker, "lua_http", WORKER_STACK_BYTES, request, WORKER_PRIORITY, nullptr) != pdPASS) {
        release_request(request);
        return push_failure(state, "could not start the HTTP task");
    }
#else
    release_request(request);
    return push_failure(state, "HTTP is not available in the simulator");
#endif

    return 1;
}

void deliver_result(HttpRequest* request) {
    lua_State* state = request->state;
    lua_rawgeti(state, LUA_REGISTRYINDEX, request->callback_ref);
    if (lua_type(state, -1) == LUA_TFUNCTION) {
        if (request->cancelled) {
            lua_pushnil(state);
            lua_pushstring(state, "cancelled");
            if (lua_pcall(state, 2, 0, 0) != LUA_OK) {
                LOG_E(TAG, "HTTP callback: %s", lua_tostring(state, -1));
                lua_pop(state, 1);
            }
        } else if (request->error[0] != '\0') {
            lua_pushnil(state);
            lua_pushstring(state, request->error);
            if (lua_pcall(state, 2, 0, 0) != LUA_OK) {
                LOG_E(TAG, "HTTP callback: %s", lua_tostring(state, -1));
                lua_pop(state, 1);
            }
        } else {
            push_response(state, request);
            if (lua_pcall(state, 1, 0, 0) != LUA_OK) {
                LOG_E(TAG, "HTTP callback: %s", lua_tostring(state, -1));
                lua_pop(state, 1);
            }
        }
    } else {
        lua_pop(state, 1);
    }

    release_request(request);
}

/** `tactility.http.Request:cancel()` - asks the worker to stop early. */
int http_cancel(lua_State* state) {
    auto* handle = static_cast<HttpHandle*>(luaL_checkudata(state, 1, HTTP_METATABLE));
    if (handle->request != nullptr && !handle->request->finished) {
        handle->request->cancel_requested.store(true, std::memory_order_relaxed);
    }
    lua_pushboolean(state, 1);
    return 1;
}

/** `tactility.http.Request:is_active()` - true until the callback has run. */
int http_is_active(lua_State* state) {
    auto* handle = static_cast<HttpHandle*>(luaL_checkudata(state, 1, HTTP_METATABLE));
    lua_pushboolean(state, handle->request != nullptr && !handle->request->finished);
    return 1;
}

int http_gc(lua_State* state) {
    auto* handle = static_cast<HttpHandle*>(luaL_checkudata(state, 1, HTTP_METATABLE));
    if (handle->request != nullptr) {
        if (!handle->request->abandoned) {
            release_request(handle->request);
            free_request(handle->request);
        }
        handle->request = nullptr;
    }
    return 0;
}

const luaL_Reg http_methods[] = {
    { "cancel", http_cancel },
    { "is_active", http_is_active },
    { nullptr, nullptr }
};

const luaL_Reg http_functions[] = {
    { "request", http_request },
    { nullptr, nullptr }
};

void add_http_api(lua_State* state) {
    lua_getglobal(state, "tactility");
    if (!lua_istable(state, -1)) {
        lua_pop(state, 1);
        LOG_E(TAG, "tactility table is missing");
        return;
    }

    luaL_newlib(state, http_functions);
    lua_setfield(state, -2, "http");
    lua_pop(state, 1);
}

} // namespace

void luavgl_open_http(lua_State* state) {
    luaL_newmetatable(state, HTTP_METATABLE);
    luaL_newlib(state, http_methods);
    lua_setfield(state, -2, "__index");
    lua_pushcfunction(state, http_gc);
    lua_setfield(state, -2, "__gc");
    lua_pushboolean(state, 0);
    lua_setfield(state, -2, "__metatable");
    lua_pop(state, 1);

    add_http_api(state);
    luavgl_open_downloads(state);
}

extern "C" void luavgl_downloads_poll(struct LuaRuntime* runtime);

extern "C" extern "C" void luavgl_http_poll(struct LuaRuntime* runtime) {
    auto* state = static_cast<lua_State*>(lua_runtime_get_state(runtime));
    push_requests(state);

    HttpRequest* requests[MAX_CONCURRENT + 4];
    size_t count = 0;

    lua_pushnil(state);
    while (lua_next(state, -2) != 0) {
        lua_pop(state, 1); // handle
        if (count < sizeof(requests) / sizeof(requests[0])) {
            requests[count++] = static_cast<HttpRequest*>(lua_touserdata(state, -1));
        }
    }
    lua_pop(state, 1);

    for (size_t i = 0; i < count; i++) {
        HttpRequest* request = requests[i];
        if (request == nullptr || request->finished) {
            continue;
        }
        if (!request->done.load(std::memory_order_acquire)) {
            continue;
        }
        deliver_result(request);
    }

    luavgl_downloads_poll(runtime);
}

void luavgl_delete_all_http(lua_State* state) {
    push_requests(state);

    HttpRequest* requests[MAX_CONCURRENT + 4];
    size_t count = 0;
    lua_pushnil(state);
    while (lua_next(state, -2) != 0) {
        lua_pop(state, 1); // handle
        if (count < sizeof(requests) / sizeof(requests[0])) {
            requests[count++] = static_cast<HttpRequest*>(lua_touserdata(state, -1));
        }
    }
    lua_pop(state, 1);

    for (size_t i = 0; i < count; i++) {
        abandon_request(requests[i]);
    }

    luavgl_delete_all_downloads(state);
}
