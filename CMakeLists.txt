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

option (CUSHION_WARNINGS_AS_ERRORS "Whether Cushion uses warnings as errors during building." OFF)
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
                -Wno-implicit-fallthrough
                # Zero length arrays greatly increase readability for classes and structs with dynamic sizes.
                -Wno-zero-length-array)
    endif ()
endif ()

# Add our subdirectories.

add_subdirectory (executable)
add_subdirectory (library)

if (CUSHION_TEST)
    add_subdirectory(test)
endif ()

# Provide install logic.

install (TARGETS cushion lib_cushion EXPORT cushion)
install (FILES "${CMAKE_CURRENT_SOURCE_DIR}/library/cushion.h" DESTINATION "include/cushion")
install (EXPORT cushion DESTINATION cushion)
