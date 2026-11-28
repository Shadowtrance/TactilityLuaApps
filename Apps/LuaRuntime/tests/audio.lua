-- WAV playback through the audio stream device. The file is pushed by the
-- device-test setup; a device without an output codec reports a skip.

local audio = tactility.audio
assert(type(audio) == "table", "tactility.audio")

local status = audio.status()
assert(type(status) == "table", "audio.status")
if not status.available or not status.output then
    result = "audio PASS: skipped (no output)"
    tactility.log.info("SWEEPRESULT", result)
    return
end

if status.enabled == false then
    result = "audio PASS: skipped (output disabled)"
    tactility.log.info("SWEEPRESULT", result)
    return
end

local player, reason = audio.play_file({ path = "/sdcard/audio_test.wav", volume = 60 })
if player == nil then
    result = "audio FAIL: " .. tostring(reason)
    tactility.log.info("SWEEPRESULT", result)
    return
end

assert(player:is_active(), "player is active")
assert(player:duration() > 0.5, "duration: " .. tostring(player:duration()))

local timer
timer = lvgl.Timer{
    period = 300,
    callback = function()
        if player:is_active() then
            return
        end

        timer:delete()
        local error = player:error()
        if error ~= nil then
            result = "audio FAIL: " .. error
        else
            result = string.format(
                "audio PASS: %.2f / %.2fs, volume %d",
                player:position(),
                player:duration(),
                player:volume()
            )
        end
        tactility.log.info("SWEEPRESULT", result)
    end,
}

result = "audio started"
