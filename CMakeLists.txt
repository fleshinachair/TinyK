cmake_minimum_required(VERSION 3.15)
project(schwung-mk-va VERSION 1.0.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Source files
set(SOURCES
    src/dsp/dsp.c
)

# Move plugin shared library: builds dsp.so
add_library(mk-va-plugin SHARED ${SOURCES})
add_library(jp8000-move-plugin ALIAS mk-va-plugin)

target_include_directories(mk-va-plugin PRIVATE
    src
    src/dsp
)

target_compile_options(mk-va-plugin PRIVATE
    -Wall -Wextra -O3 -fPIC
)

if(NOT MSVC)
    target_link_libraries(mk-va-plugin PRIVATE m)
endif()

if(NOT APPLE AND NOT WIN32)
    target_link_options(mk-va-plugin PRIVATE
        -Wl,--exclude-libs,ALL)
endif()

set_target_properties(mk-va-plugin PROPERTIES
    OUTPUT_NAME "dsp"
    PREFIX ""
    SUFFIX ".so"
)
