add_executable (cushion main.c)
target_link_libraries (cushion PRIVATE lib_cushion)

if ($ENV{CUSHION_BUILD_VERSION})
    target_compile_definitions (cushion PRIVATE "CUSHION_VERSION_STRING=\"$ENV{CUSHION_BUILD_VERSION}\"")
else ()
    target_compile_definitions (cushion PRIVATE "CUSHION_VERSION_STRING=\"(custom build)\"")
endif ()
