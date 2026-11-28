// SPDX-License-Identifier: Apache-2.0
#include "bindings_private.h"

#include <tactility/device.h>
#include <tactility/drivers/audio_codec.h>
#include <tactility/drivers/audio_stream.h>
#include <tactility/error.h>
#include <tactility/log.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <new>

#ifdef ESP_PLATFORM
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#endif

extern "C" {
#include <lauxlib.h>
}

namespace {

constexpr auto* TAG = "lua.audio";
constexpr auto* PLAYER_METATABLE = "tactility.audio.Player";
constexpr size_t BUFFER_SIZE = 4096;
#ifdef ESP_PLATFORM
constexpr uint32_t STACK_BYTES = 12288;
constexpr UBaseType_t TASK_PRIORITY = 5;
#endif

/** A PCM WAV player, owned by its Lua handle and run by one task. */
struct AudioPlayer {
    Device* device = nullptr;
    AudioStreamHandle stream = nullptr;
    FILE* file = nullptr;

    uint32_t sample_rate = 0;
    uint16_t bits_per_sample = 0;
    uint8_t channels = 0;
    uint32_t byte_rate = 0;
    long data_offset = 0;
    uint32_t data_size = 0;
    bool loop = false;

    std::atomic<bool> stop_requested{false};
    std::atomic<bool> paused{false};
    std::atomic<bool> finished{false};
    std::atomic<uint32_t> position_ms{0};
    std::atomic<uint32_t> duration_ms{0};
    char error[96] = {};
};

struct AudioHandle {
    AudioPlayer* player = nullptr;
};

int push_failure(lua_State* state, const char* message) {
    lua_pushnil(state);
    lua_pushstring(state, message);
    return 2;
}

uint16_t read_u16(const uint8_t* data) {
    return static_cast<uint16_t>(data[0] | (data[1] << 8));
}

uint32_t read_u32(const uint8_t* data) {
    return static_cast<uint32_t>(data[0]) |
        (static_cast<uint32_t>(data[1]) << 8) |
        (static_cast<uint32_t>(data[2]) << 16) |
        (static_cast<uint32_t>(data[3]) << 24);
}

/** Finds the output device and checks that a codec is bound for it. */
error_t find_output_device(Device** out_device, bool* out_supported) {
    Device* device = nullptr;
    error_t error = device_get_first_active_by_type(&AUDIO_STREAM_TYPE, &device);
    if (error != ERROR_NONE) {
        return error;
    }

    bool supported = false;
    error = audio_stream_is_supported(device, AUDIO_CODEC_DIR_OUTPUT, &supported);
    if (error != ERROR_NONE || !supported) {
        device_put(device);
        return error != ERROR_NONE ? error : ERROR_NOT_SUPPORTED;
    }

    *out_device = device;
    *out_supported = supported;
    return ERROR_NONE;
}

/** Parses the chunks of a PCM WAV file. Leaves the file open at the data chunk. */
error_t parse_wav(AudioPlayer* player, const char* path) {
    player->file = std::fopen(path, "rb");
    if (player->file == nullptr) {
        return ERROR_NOT_FOUND;
    }

    uint8_t riff[12];
    if (std::fread(riff, 1, sizeof(riff), player->file) != sizeof(riff) ||
        std::memcmp(riff, "RIFF", 4) != 0 ||
        std::memcmp(riff + 8, "WAVE", 4) != 0) {
        return ERROR_INVALID_ARGUMENT;
    }

    uint16_t format = 0;
    bool have_format = false;

    while (true) {
        uint8_t header[8];
        if (std::fread(header, 1, sizeof(header), player->file) != sizeof(header)) {
            return ERROR_INVALID_ARGUMENT;
        }

        const uint32_t size = read_u32(header + 4);
        if (std::memcmp(header, "fmt ", 4) == 0) {
            uint8_t fmt[16];
            if (size < sizeof(fmt) || std::fread(fmt, 1, sizeof(fmt), player->file) != sizeof(fmt)) {
                return ERROR_INVALID_ARGUMENT;
            }
            format = read_u16(fmt);
            player->channels = static_cast<uint8_t>(read_u16(fmt + 2));
            player->sample_rate = read_u32(fmt + 4);
            player->byte_rate = read_u32(fmt + 8);
            player->bits_per_sample = read_u16(fmt + 14);
            have_format = true;

            const long remaining = static_cast<long>(size) - static_cast<long>(sizeof(fmt));
            if (remaining > 0 && std::fseek(player->file, remaining + (remaining & 1), SEEK_CUR) != 0) {
                return ERROR_INVALID_ARGUMENT;
            }
        } else if (std::memcmp(header, "data", 4) == 0) {
            if (!have_format) {
                return ERROR_INVALID_ARGUMENT;
            }
            player->data_offset = std::ftell(player->file);
            player->data_size = size;
            break;
        } else {
            const long skip = static_cast<long>(size) + (size & 1);
            if (std::fseek(player->file, skip, SEEK_CUR) != 0) {
                return ERROR_INVALID_ARGUMENT;
            }
        }
    }

    if (format != 1 || player->channels == 0 || player->channels > 2 ||
        player->sample_rate == 0 ||
        (player->bits_per_sample != 16 && player->bits_per_sample != 24 && player->bits_per_sample != 32)) {
        return ERROR_NOT_SUPPORTED;
    }

    if (player->byte_rate == 0) {
        player->byte_rate = player->sample_rate * player->channels * player->bits_per_sample / 8;
    }
    if (player->data_size == 0) {
        const long current = std::ftell(player->file);
        if (current < 0 || std::fseek(player->file, 0, SEEK_END) != 0) {
            return ERROR_INVALID_ARGUMENT;
        }
        const long end = std::ftell(player->file);
        if (end <= player->data_offset) {
            return ERROR_INVALID_ARGUMENT;
        }
        player->data_size = static_cast<uint32_t>(end - player->data_offset);
        std::fseek(player->file, current, SEEK_SET);
    }
    if (player->data_size == 0 || player->byte_rate == 0) {
        return ERROR_INVALID_ARGUMENT;
    }

    player->duration_ms.store(
        static_cast<uint32_t>(static_cast<uint64_t>(player->data_size) * 1000 / player->byte_rate),
        std::memory_order_relaxed
    );
    return ERROR_NONE;
}

void set_error(AudioPlayer* player, const char* message) {
    if (player->error[0] == '\0') {
        std::snprintf(player->error, sizeof(player->error), "%s", message);
    }
}

#ifdef ESP_PLATFORM
void player_task(void* argument) {
    auto* player = static_cast<AudioPlayer*>(argument);

    auto* buffer = static_cast<uint8_t*>(std::malloc(BUFFER_SIZE));
    if (buffer == nullptr) {
        set_error(player, "out of memory");
        player->finished.store(true, std::memory_order_release);
        vTaskDelete(nullptr);
        return;
    }

    AudioStreamConfig config = {
        .sample_rate = player->sample_rate,
        .bits_per_sample = static_cast<uint8_t>(player->bits_per_sample),
        .channels = player->channels,
    };
    error_t error = audio_stream_open_output(player->device, &config, &player->stream);
    if (error != ERROR_NONE) {
        set_error(player, error_to_string(error));
        std::free(buffer);
        player->finished.store(true, std::memory_order_release);
        vTaskDelete(nullptr);
        return;
    }

    if (std::fseek(player->file, player->data_offset, SEEK_SET) != 0) {
        set_error(player, "could not seek to the audio data");
    }

    uint32_t written_bytes = 0;

    while (player->error[0] == '\0' && !player->stop_requested.load(std::memory_order_relaxed)) {
        if (player->paused.load(std::memory_order_relaxed)) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        uint32_t remaining = 0;
        if (player->data_size > written_bytes) {
            remaining = player->data_size - written_bytes;
        } else if (player->loop) {
            if (std::fseek(player->file, player->data_offset, SEEK_SET) != 0) {
                set_error(player, "could not restart the audio data");
                break;
            }
            written_bytes = 0;
            remaining = player->data_size;
        } else {
            break;
        }

        size_t want = BUFFER_SIZE;
        if (want > remaining) {
            want = remaining;
        }

        const size_t read = std::fread(buffer, 1, want, player->file);
        if (read == 0) {
            if (player->loop) {
                if (std::fseek(player->file, player->data_offset, SEEK_SET) != 0) {
                    set_error(player, "could not restart the audio data");
                    break;
                }
                written_bytes = 0;
                continue;
            }
            break;
        }

        size_t offset = 0;
        while (offset < read && !player->stop_requested.load(std::memory_order_relaxed)) {
            size_t written = 0;
            error = audio_stream_write(
                player->stream,
                buffer + offset,
                read - offset,
                &written,
                pdMS_TO_TICKS(500)
            );
            if (error != ERROR_NONE) {
                set_error(player, error_to_string(error));
                break;
            }
            offset += written;
            written_bytes += static_cast<uint32_t>(written);
            player->position_ms.store(
                static_cast<uint32_t>(static_cast<uint64_t>(written_bytes) * 1000 / player->byte_rate),
                std::memory_order_relaxed
            );
        }
    }

    std::free(buffer);
    if (player->stream != nullptr) {
        audio_stream_close(player->stream);
        player->stream = nullptr;
    }
    player->finished.store(true, std::memory_order_release);
    vTaskDelete(nullptr);
}
#endif

void player_request_stop(AudioPlayer* player) {
    if (player != nullptr) {
        player->stop_requested.store(true, std::memory_order_relaxed);
    }
}

/** Waits for the task to finish. Returns false on timeout. */
bool player_wait(AudioPlayer* player, uint32_t timeout_ms) {
#ifdef ESP_PLATFORM
    const uint32_t step = 10;
    for (uint32_t waited = 0; waited < timeout_ms; waited += step) {
        if (player->finished.load(std::memory_order_acquire)) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(step));
    }
#else
    (void)timeout_ms;
#endif
    return player->finished.load(std::memory_order_acquire);
}

void player_free(AudioPlayer* player) {
    if (player == nullptr) {
        return;
    }
    if (player->file != nullptr) {
        std::fclose(player->file);
        player->file = nullptr;
    }
    if (player->device != nullptr) {
        device_put(player->device);
        player->device = nullptr;
    }
    delete player;
}

/** `tactility.audio.status()` */
int audio_status(lua_State* state) {
    Device* device = nullptr;
    bool supported = false;
    const error_t error = find_output_device(&device, &supported);

    lua_createtable(state, 0, 6);
    lua_pushboolean(state, error == ERROR_NONE && supported);
    lua_setfield(state, -2, "available");
    lua_pushboolean(state, error == ERROR_NONE && supported);
    lua_setfield(state, -2, "output");

    if (error == ERROR_NONE) {
        bool enabled = false;
        float volume = 0.0f;
        bool muted = false;
        const bool have_enabled = audio_stream_get_enabled(device, AUDIO_CODEC_DIR_OUTPUT, &enabled) == ERROR_NONE;
        const bool have_volume = audio_stream_get_volume(device, AUDIO_CODEC_DIR_OUTPUT, &volume) == ERROR_NONE;
        const bool have_mute = audio_stream_get_mute(device, AUDIO_CODEC_DIR_OUTPUT, &muted) == ERROR_NONE;

        lua_pushboolean(state, have_enabled && enabled);
        lua_setfield(state, -2, "enabled");
        if (have_volume) {
            lua_pushinteger(state, static_cast<lua_Integer>(volume + 0.5f));
        } else {
            lua_pushnil(state);
        }
        lua_setfield(state, -2, "volume");
        lua_pushboolean(state, have_mute && muted);
        lua_setfield(state, -2, "muted");
        lua_pushstring(state, device->name);
        lua_setfield(state, -2, "device");
        device_put(device);
    }

    return 1;
}

/** `tactility.audio.play_file(options) -> handle | nil, reason` */
int audio_play_file(lua_State* state) {
    luaL_checktype(state, 1, LUA_TTABLE);

#ifdef ESP_PLATFORM
    lua_getfield(state, 1, "path");
    if (!lua_isstring(state, -1)) {
        lua_pop(state, 1);
        return push_failure(state, "path is required");
    }
    const char* path = lua_tostring(state, -1);

    Device* device = nullptr;
    bool supported = false;
    const error_t device_error = find_output_device(&device, &supported);
    if (device_error != ERROR_NONE || !supported) {
        lua_pop(state, 1);
        return push_failure(state, "no audio output");
    }

    auto* player = new (std::nothrow) AudioPlayer();
    if (player == nullptr) {
        lua_pop(state, 1);
        device_put(device);
        return push_failure(state, "out of memory");
    }
    player->device = device;

    const error_t parse_error = parse_wav(player, path);
    lua_pop(state, 1);
    if (parse_error != ERROR_NONE) {
        player_free(player);
        return push_failure(
            state,
            parse_error == ERROR_NOT_FOUND ? "could not open the audio file"
                : (parse_error == ERROR_NOT_SUPPORTED ? "only 16/24/32-bit PCM WAV is supported" : "invalid WAV file")
        );
    }

    lua_getfield(state, 1, "loop");
    player->loop = lua_toboolean(state, -1) != 0;
    lua_pop(state, 1);

    lua_getfield(state, 1, "volume");
    if (lua_isnumber(state, -1)) {
        const float volume = static_cast<float>(lua_tonumber(state, -1));
        audio_stream_set_volume(device, AUDIO_CODEC_DIR_OUTPUT, volume < 0 ? 0 : (volume > 100 ? 100 : volume));
    }
    lua_pop(state, 1);

    auto* handle = static_cast<AudioHandle*>(lua_newuserdatauv(state, sizeof(AudioHandle), 0));
    handle->player = player;
    luaL_setmetatable(state, PLAYER_METATABLE);

    if (xTaskCreate(player_task, "lua_audio", STACK_BYTES, player, TASK_PRIORITY, nullptr) != pdPASS) {
        handle->player = nullptr;
        player_free(player);
        return push_failure(state, "could not start the audio task");
    }

    return 1;
#else
    return push_failure(state, "audio playback is not available in the simulator");
#endif
}

AudioPlayer* check_player(lua_State* state) {
    auto* handle = static_cast<AudioHandle*>(luaL_checkudata(state, 1, PLAYER_METATABLE));
    if (handle->player == nullptr) {
        luaL_error(state, "audio player has been released");
    }
    return handle->player;
}

int player_stop(lua_State* state) {
    player_request_stop(check_player(state));
    lua_pushboolean(state, 1);
    return 1;
}

int player_pause(lua_State* state) {
    auto* player = check_player(state);
    if (!player->finished.load(std::memory_order_acquire)) {
        player->paused.store(true, std::memory_order_relaxed);
    }
    lua_pushboolean(state, 1);
    return 1;
}

int player_resume(lua_State* state) {
    auto* player = check_player(state);
    player->paused.store(false, std::memory_order_relaxed);
    lua_pushboolean(state, 1);
    return 1;
}

int player_is_active(lua_State* state) {
    auto* player = check_player(state);
    lua_pushboolean(state, !player->finished.load(std::memory_order_acquire));
    return 1;
}

int player_position(lua_State* state) {
    auto* player = check_player(state);
    lua_pushnumber(state, static_cast<lua_Number>(player->position_ms.load(std::memory_order_relaxed)) / 1000.0);
    return 1;
}

int player_duration(lua_State* state) {
    auto* player = check_player(state);
    lua_pushnumber(state, static_cast<lua_Number>(player->duration_ms.load(std::memory_order_relaxed)) / 1000.0);
    return 1;
}

int player_volume(lua_State* state) {
    auto* player = check_player(state);

    if (!lua_isnoneornil(state, 2)) {
        float volume = static_cast<float>(luaL_checknumber(state, 2));
        volume = volume < 0 ? 0 : (volume > 100 ? 100 : volume);
        const error_t error = audio_stream_set_volume(player->device, AUDIO_CODEC_DIR_OUTPUT, volume);
        if (error != ERROR_NONE) {
            return push_failure(state, error_to_string(error));
        }
    }

    float volume = 0;
    const error_t error = audio_stream_get_volume(player->device, AUDIO_CODEC_DIR_OUTPUT, &volume);
    if (error != ERROR_NONE) {
        return push_failure(state, error_to_string(error));
    }
    lua_pushinteger(state, static_cast<lua_Integer>(volume + 0.5f));
    return 1;
}

int player_error(lua_State* state) {
    auto* player = check_player(state);
    if (player->error[0] == '\0') {
        lua_pushnil(state);
    } else {
        lua_pushstring(state, player->error);
    }
    return 1;
}

int player_gc(lua_State* state) {
    auto* handle = static_cast<AudioHandle*>(luaL_checkudata(state, 1, PLAYER_METATABLE));
    if (handle->player != nullptr) {
        AudioPlayer* player = handle->player;
        player_request_stop(player);
        if (player_wait(player, 2000)) {
            player_free(player);
        } else {
            // The task is still writing; leaking the player is safer than freeing it under the task.
            LOG_E(TAG, "audio task did not stop in time");
        }
        handle->player = nullptr;
    }
    return 0;
}

const luaL_Reg player_methods[] = {
    { "stop", player_stop },
    { "pause", player_pause },
    { "resume", player_resume },
    { "is_active", player_is_active },
    { "position", player_position },
    { "duration", player_duration },
    { "volume", player_volume },
    { "error", player_error },
    { nullptr, nullptr }
};

const luaL_Reg audio_functions[] = {
    { "status", audio_status },
    { "play_file", audio_play_file },
    { nullptr, nullptr }
};

} // namespace

void lua_bindings_open_audio(lua_State* state) {
    luaL_newmetatable(state, PLAYER_METATABLE);
    luaL_newlib(state, player_methods);
    lua_setfield(state, -2, "__index");
    lua_pushcfunction(state, player_gc);
    lua_setfield(state, -2, "__gc");
    lua_pushboolean(state, 0);
    lua_setfield(state, -2, "__metatable");
    lua_pop(state, 1);

    luaL_newlib(state, audio_functions);
    lua_setfield(state, -2, "audio"); // tactility.audio
}
