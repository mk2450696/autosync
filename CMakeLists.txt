cmake_minimum_required(VERSION 3.15)
project(AutoPacer CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

add_library(AutoPacer SHARED src/dllmain.cpp)

target_compile_definitions(AutoPacer PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

target_compile_options(AutoPacer PRIVATE
    /W3 /O2 /GL /arch:SSE2 /GS- /Gy
)

target_link_libraries(AutoPacer PRIVATE
    dxgi.lib
    d3d11.lib
    dxguid.lib
    user32.lib
)

target_link_options(AutoPacer PRIVATE /LTCG)

set_target_properties(AutoPacer PROPERTIES
    OUTPUT_NAME "AutoPacer"
    SUFFIX ".dll"
)
