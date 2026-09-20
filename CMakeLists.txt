cmake_minimum_required(VERSION 3.20)
project(VantaESP LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Only MSVC / ClangCL on Windows
if(NOT WIN32)
    message(FATAL_ERROR "Windows only.")
endif()

add_library(VantaESP SHARED
    src/dllmain.cpp
    src/pch.h
)

target_precompile_headers(VantaESP PRIVATE src/pch.h)

target_compile_definitions(VantaESP PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
)

target_compile_options(VantaESP PRIVATE
    /W3
    /O2
    /GS-          # disable buffer security check (less noise in scan)
    /Oy           # omit frame pointers
    /GL           # whole program optimization
)

target_link_options(VantaESP PRIVATE
    /LTCG
    /SUBSYSTEM:WINDOWS
)

target_link_libraries(VantaESP PRIVATE
    gdiplus
    dwmapi
    d3d11        # present hook landing pad if needed later
)

set_target_properties(VantaESP PROPERTIES
    OUTPUT_NAME "VantaESP"
    SUFFIX ".dll"
    PDB_NAME "VantaESP"
)
