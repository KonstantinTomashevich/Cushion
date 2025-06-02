set (TOKENIZATION_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/source/tokenization.c")
set (TOKENIZATION_SOURCE_PREPROCESSED "${CMAKE_CURRENT_BINARY_DIR}/tokenization.c")

if (CUSHION_IDENTIFIERS_UNICODE)
    set (RE2C_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/re2c/unicode")
else ()
    set (RE2C_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/re2c/ascii")
endif ()

add_custom_command (
        OUTPUT "${TOKENIZATION_SOURCE_PREPROCESSED}"
        COMMAND
        "${CUSHION_RE2C_EXECUTABLE}"
        "${TOKENIZATION_SOURCE}"
        -o "${TOKENIZATION_SOURCE_PREPROCESSED}"
        --depfile "${CMAKE_CURRENT_BINARY_DIR}/tokenization.depfile"
        -I "${RE2C_INCLUDE_DIR}"
        DEPENDS "${TOKENIZATION_SOURCE}"
        DEPFILE "${CMAKE_CURRENT_BINARY_DIR}/tokenization.depfile"
        COMMENT "Preprocess re2c syntax inside Cushion tokenization implementation."
        VERBATIM)

# Add compile definitions globally in directory, as we have a highlight target too.
if (CUSHION_EXTENSIONS)
    add_compile_definitions (CUSHION_EXTENSIONS)
endif ()

add_compile_definitions (
        "CUSHION_ALLOCATOR_PAGE_SIZE=${CUSHION_ALLOCATOR_PAGE_SIZE}"
        "CUSHION_MACRO_BUCKETS=${CUSHION_MACRO_BUCKETS}"
        "CUSHION_PRAGMA_ONCE_BUCKETS=${CUSHION_PRAGMA_ONCE_BUCKETS}"
        "CUSHION_DEPFILE_BUCKETS=${CUSHION_DEPFILE_BUCKETS}"
        "CUSHION_INPUT_BUFFER_SIZE=${CUSHION_INPUT_BUFFER_SIZE}"
        "CUSHION_PATH_BUFFER_SIZE=${CUSHION_PATH_BUFFER_SIZE}")

set (CUSHION_SOURCES 
        "${CMAKE_CURRENT_SOURCE_DIR}/source/api.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/source/instance.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/source/lexing.c")

add_library (lib_cushion STATIC "${CUSHION_SOURCES}" "${TOKENIZATION_SOURCE_PREPROCESSED}")
target_include_directories (lib_cushion PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/source>
        $<INSTALL_INTERFACE:include/cushion>)

# Duplicate highlight target for proper highlight of Cushion source inside IDEs.
add_library (lib_cushion_highlight OBJECT EXCLUDE_FROM_ALL "${CUSHION_SOURCES}" "${TOKENIZATION_SOURCE}")
target_include_directories (lib_cushion_highlight PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
