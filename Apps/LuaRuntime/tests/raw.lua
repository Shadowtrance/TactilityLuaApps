-- Low-level raw dispatcher smoke test.
--
-- lv_tick_get() is exported by the LVGL module, takes no arguments and returns
-- an integer, which makes it a safe way to prove symbol lookup and the call
-- trampoline without touching hardware.

local first, first_error = tactility.raw.call("lv_tick_get")
assert(type(first) == "number", first_error or "lv_tick_get did not return a number")

local second = tactility.raw.call("lv_tick_get")
assert(second >= first, "tick did not advance")

local missing, reason = tactility.raw.call("definitely_not_an_exported_symbol")
assert(missing == nil, "unknown symbol should return nil")
assert(type(reason) == "string", "unknown symbol should return a reason")

result = string.format("raw PASS: tick %d -> %d, missing -> %s", first, second, reason)
