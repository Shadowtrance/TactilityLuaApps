# tactility-cmakelists-version: 1
cmake_minimum_required(VERSION 3.20)

if (NOT DEFINED ENV{TACTILITY_SDK_PATH})
    message(FATAL_ERROR "TACTILITY_SDK_PATH environment variable is not set")
endif()

get_filename_component(TACTILITY_SDK_PATH "$ENV{TACTILITY_SDK_PATH}" ABSOLUTE)
include("${TACTILITY_SDK_PATH}/TactilitySDK.cmake")

tactility_project_pre(tactility.luaruntime.lua_runtime)

# Capability glue is generated from bindings/*.toml before the component is
# configured. The output lives in main/Generated and is not committed.
if(DEFINED PYTHON)
    set(BINDINGS_PYTHON "${PYTHON}")
else()
    find_program(BINDINGS_PYTHON NAMES python3 python)
endif()
if(NOT BINDINGS_PYTHON)
    message(FATAL_ERROR "Python is required to generate the Lua capability bindings")
endif()

set(BINDINGS_IDL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../bindings")
set(BINDINGS_OUTPUT_DIR "${CMAKE_BINARY_DIR}/generated")
set(BINDINGS_GENERATOR "${CMAKE_CURRENT_SOURCE_DIR}/../tools/lua-bindings/generate_bindings.py")
file(GLOB BINDING_IDLS "${BINDINGS_IDL_DIR}/*.toml")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    ${BINDING_IDLS} "${BINDINGS_GENERATOR}")
execute_process(
    COMMAND "${BINDINGS_PYTHON}"
        "${BINDINGS_GENERATOR}"
        --idl-dir "${BINDINGS_IDL_DIR}"
        --output-dir "${BINDINGS_OUTPUT_DIR}"
    RESULT_VARIABLE BINDINGS_RESULT
    OUTPUT_VARIABLE BINDINGS_STDOUT
    ERROR_VARIABLE BINDINGS_STDERR
)
if(NOT BINDINGS_RESULT EQUAL 0)
    message(FATAL_ERROR "Lua binding generation failed:\n${BINDINGS_STDOUT}\n${BINDINGS_STDERR}")
endif()

project(tactility.luaruntime.lua_runtime)

# The external module carries its own interpreter. project_elf() links only
# "main" by default, so the Lua component has to be listed explicitly or every
# lua_* call is left undefined for the firmware resolver.
set(ELF_COMPONENTS "lua")

tactility_project_post(tactility.luaruntime.lua_runtime)
