cmake_minimum_required(VERSION 3.20)

# This copy is built as part of the external Lua runtime module, so it is always
# an ESP-IDF component (not the simulator's standalone static library).
idf_component_register(
    SRC_DIRS "Source/"
    INCLUDE_DIRS "Include/"
    PRIV_INCLUDE_DIRS "Source/"
)

# Upstream Lua is warning-clean under its own Makefile but not under IDF's stricter
# default set. These are in third-party code we do not want to patch.
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-implicit-fallthrough
    -Wno-type-limits
)
