-- JSON decode/encode round trips, arrays, null and the failure paths.

local json = tactility.json
assert(type(json) == "table", "tactility.json is missing")

local text = [[{"name":"deck","count":2,"ratio":0.5,"ok":true,"tags":["a",null,"c"],"nested":{"value":3}}]]
local value = json.decode(text)
assert(type(value) == "table", "decode")
assert(value.name == "deck")
assert(value.count == 2)
assert(value.ratio == 0.5)
assert(value.ok == true)
assert(value.tags[1] == "a")
assert(value.tags[2] == json.null, "null keeps its array slot")
assert(value.tags[3] == "c")
assert(value.nested.value == 3)

local encoded = json.encode(value)
assert(type(encoded) == "string" and #encoded > 0, "encode")
local round = json.decode(encoded)
assert(round.name == value.name and round.tags[2] == json.null, "round trip")

assert(json.encode({ 1, 2, 3 }) == "[1,2,3]", "array")
assert(json.encode({}) == "[]", "empty table is an array")
assert(json.encode({ x = json.null }) == '{"x":null}', "null object field")

local bad, reason = json.decode("{")
assert(bad == nil and type(reason) == "string" and reason:find("invalid JSON"), "invalid input")

bad, reason = json.encode({ [1] = "x", named = "y" })
assert(bad == nil and type(reason) == "string", "mixed keys are rejected")

bad, reason = json.encode("a\0b")
assert(bad == nil and type(reason) == "string", "NUL in a string is rejected")

local deep = {}
local cursor = deep
for _ = 1, 80 do
    cursor.next = {}
    cursor = cursor.next
end
bad, reason = json.encode(deep)
assert(bad == nil and tostring(reason):find("nested too deeply"), "depth limit")

result = "json PASS: decode/encode, arrays, null, errors"
