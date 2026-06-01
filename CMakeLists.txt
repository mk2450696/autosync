cmake_minimum_required(VERSION 3.20)
project(AutoPacer VERSION 10.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# DLL target
add_library(AutoPacer SHARED src/dllmain.cpp)

target_compile_definitions(AutoPacer PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _UNICODE
    UNICODE
)

target_compile_options(AutoPacer PRIVATE
    /W3
    /O2
    /arch:SSE2   # _mm_pause() for spinlock
    /GS-         # No buffer security check (slight perf win in hot path)
    /Gy          # Function-level linking
)

target_link_libraries(AutoPacer PRIVATE
    dxgi.lib
    d3d11.lib
    dxguid.lib
)

# Output as .dll; rename to .asi manually or via post-build step
set_target_properties(AutoPacer PROPERTIES
    OUTPUT_NAME "AutoPacer"
    SUFFIX ".dll"
)

# Optional: copy to a known location for testing
# set_target_properties(AutoPacer PROPERTIES
#     RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/output"
# )
