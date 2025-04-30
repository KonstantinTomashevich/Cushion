set (CUSHION_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/cushion.c")
set (CUSHION_SOURCE_PREPROCESSED "${CMAKE_CURRENT_BINARY_DIR}/cushion.c")

add_custom_command (
        OUTPUT "${CUSHION_SOURCE_PREPROCESSED}"
        COMMAND "${CUSHION_RE2C_EXECUTABLE}" "${CUSHION_SOURCE}" -o "${CUSHION_SOURCE_PREPROCESSED}"
        DEPENDS "${CUSHION_SOURCE}"
        COMMENT "Preprocess re2c syntax inside Cushion implementation."
        VERBATIM)

# Add compile definitions globally in directory, as we have a highlight target too.
if (CUSHION_EXTENSIONS)
    add_compile_definitions (CUSHION_EXTENSIONS)
endif ()

add_compile_definitions (
        "CUSHION_ALLOCATOR_PAGE_SIZE=${CUSHION_ALLOCATOR_PAGE_SIZE}"
        "CUSHION_MACRO_BUCKETS=${CUSHION_MACRO_BUCKETS}")

add_library (lib_cushion STATIC "${CUSHION_SOURCE_PREPROCESSED}")
target_include_directories (lib_cushion PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<INSTALL_INTERFACE:include/cushion>)

# Duplicate highlight target for proper highlight of Cushion source inside IDEs.
add_library (lib_cushion_highlight OBJECT EXCLUDE_FROM_ALL "${CUSHION_SOURCE}")
target_include_directories (lib_cushion_highlight PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
