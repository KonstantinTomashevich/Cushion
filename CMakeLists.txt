cmake_minimum_required (VERSION 3.25)
cmake_policy (VERSION 3.25)

project (
        Cushion
        DESCRIPTION "C preprocessor for feeding the code to code parsers and generators"
        LANGUAGES C)

set (CMAKE_C_STANDARD 11)

# Common options.

option (CUSHION_TEST "Whether tests for Cushion are being built." OFF)
option (CUSHION_EXTENSIONS "Whether Cushion library is built with extension support." OFF)

# Implementation constants.

set (CUSHION_ALLOCATOR_PAGE_SIZE "1048576" CACHE STRING "Size of an internal allocator inside cushion context.")
set (CUSHION_MACRO_BUCKETS "1024" CACHE STRING "Count of buckets for macro search hash map.")
set (CUSHION_PRAGMA_ONCE_BUCKETS "128" CACHE STRING "Count of buckets for pragma once file hash map.")
set (CUSHION_DEPFILE_BUCKETS "128" CACHE STRING "Count of buckets for depfile dependencies hash map.")
set (CUSHION_INPUT_BUFFER_SIZE "16384" CACHE STRING
        "Size of a buffer for input tokenization. Lexemes must not be bigger than this size.")
set (CUSHION_PATH_BUFFER_SIZE "4096" CACHE STRING "Size of a buffer for building included file paths.")
set (CUSHION_OUTPUT_FORMATTED_BUFFER_SIZE "1024" CACHE STRING "Size of a buffer for formatted output.")
set (CUSHION_OUTPUT_BUFFER_NODE_SIZE "16384" CACHE STRING 
        "Size of a buffer node for deferred output buffering. Should only be needed if extensions are enabled.")

# re2c search logic.

option (CUSHION_RE2C_SEARCH_FOR_TARGET "Search for re2c target in build tree." ON)
option (CUSHION_RE2C_SEARCH_FOR_PROGRAM "Search for re2c as program in system path." ON)

# CUSHION_RE2C_EXECUTABLE is allowed to be set from outside project when Cushion is used through add_subdirectory.

if (NOT DEFINED CUSHION_RE2C_EXECUTABLE)
    if (CUSHION_RE2C_SEARCH_FOR_TARGET AND TARGET re2c)
        set (CUSHION_RE2C_EXECUTABLE "re2c")
        message (STATUS "Cushion: using re2c target from build tree.")
    endif ()
endif ()

if (NOT DEFINED CUSHION_RE2C_EXECUTABLE)
    if (CUSHION_RE2C_SEARCH_FOR_PROGRAM)
        find_program (CUSHION_RE2C_EXECUTABLE_PATH "re2c")
        if (CUSHION_RE2C_EXECUTABLE_PATH)
            message (STATUS "Cushion: found re2c installation.")
            set (CUSHION_RE2C_EXECUTABLE "${CUSHION_RE2C_EXECUTABLE_PATH}")
        else ()
            unset (CUSHION_RE2C_EXECUTABLE)
        endif ()
    endif ()
endif ()

if (NOT DEFINED CUSHION_RE2C_EXECUTABLE)
    message (FATAL_ERROR "Cushion: re2c is not available, unable to build Cushion.")
endif ()

# Treat warnings as errors logic.

option (CUSHION_WARNINGS_AS_ERRORS "Whether Cushion uses warnings as errors during building." ON)
if (CUSHION_WARNINGS_AS_ERRORS)
    if ("${CMAKE_CXX_COMPILER_ID}" MATCHES "^.*Clang$")
        if (MSVC)
            add_compile_options (/W4 /WX)
        else ()
            add_compile_options (-Wall -Wextra -Werror -pedantic)
        endif ()

        add_compile_options (
                # Zero length arrays greatly increase readability for classes and structs with dynamic sizes.
                -Wno-zero-length-array)

    elseif (MSVC)
        add_compile_options (
                /W4
                /WX
                # Zero length arrays greatly increase readability for classes and structs with dynamic sizes.
                /wd4200
                # Currently we're okay with assignments on conditional expressions.
                /wd4706)
    else ()
        add_compile_options (
                -Wall
                -Wextra
                -Werror
                # We're aware that fallthrough is default implicit behavior and we use it in some cases.
                -Wno-implicit-fallthrough)
    endif ()
endif ()

# Add our subdirectories.

add_subdirectory (executable)
add_subdirectory (library)

if (CUSHION_TEST)
    enable_testing ()
    add_subdirectory (test)
endif ()

# Provide install logic.

install (TARGETS cushion lib_cushion EXPORT cushion)
install (FILES "${CMAKE_CURRENT_SOURCE_DIR}/library/include/cushion.h" DESTINATION "include/cushion")
install (EXPORT cushion DESTINATION cushion)

# Attach clang format if found.

find_program (CLANG_FORMAT clang-format)
if (NOT CLANG_FORMAT-NOTFOUND)
    message (STATUS "Found clang-format: \"${CLANG_FORMAT}\".")
    message (STATUS "Generating formatting targets...")

    message (STATUS "    Searching for sources...")
    file (GLOB_RECURSE FILES "executable/*.c" "executable/*.h" "library/*.c" "library/*.h")

    message (STATUS "    Writing file list...")
    list (JOIN FILES "\n" FILE_LIST_CONTENT)
    file (WRITE "${CMAKE_BINARY_DIR}/cushion_clang_format_file_list.txt" "${FILE_LIST_CONTENT}")

    message (STATUS "    Setting up targets...")
    add_custom_target (
            cushion_format_check
            COMMENT "Check formatting using clang format."
            COMMAND
            "${CLANG_FORMAT}" --style=file --dry-run --Werror 
            "--files=${CMAKE_BINARY_DIR}/cushion_clang_format_file_list.txt")

    add_custom_target (
            cushion_format_fix
            COMMENT "Fix formatting using clang format."
            COMMAND
            "${CLANG_FORMAT}" --style=file -i --Werror 
            "--files=${CMAKE_BINARY_DIR}/cushion_clang_format_file_list.txt")

else ()
    message (WARNING "clang-format not found, formatting targets won't be created.")
endif ()
