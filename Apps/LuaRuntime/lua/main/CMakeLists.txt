include("$ENV{TACTILITY_SDK_PATH}/TactilitySDK.cmake")

file(GLOB_RECURSE SOURCE_FILES Source/*.c*)

# The runtime's public header carries LUA_RUNTIME_API_VERSION, which is written
# into the packages this companion exports on-device. Header only: the
# companion does not link the interpreter.
tactility_component_register(
    SRCS ${SOURCE_FILES}
    INCLUDE_DIRS ../../lua_runtime/main/Include
)
