include("$ENV{TACTILITY_SDK_PATH}/TactilitySDK.cmake")

file(GLOB_RECURSE SOURCE_FILES Source/*.c Source/*.cpp)

# The runner links against the `lua_runtime` package module at load time
# (lua_runtime_*, lua_bindings_open and the Lua C API). It must not link
# liblua.a itself, or it would carry a second interpreter. Headers only.
tactility_component_register(
    SRCS ${SOURCE_FILES}
    INCLUDE_DIRS
        Include
        Private
        ../../lua_runtime/main/Include
        ../../lua_runtime/components/lua/Include
    PRIV_REQUIRES notification-module
)
