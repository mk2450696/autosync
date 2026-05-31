cmake_minimum_required(VERSION 3.15)
project(AutoPacer)

# Force Static C++ Runtime (No external dependencies)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

find_package(minhook CONFIG REQUIRED)
add_library(AutoPacer SHARED dllmain.cpp)
target_link_libraries(AutoPacer PRIVATE minhook::minhook d3d11.lib dxgi.lib user32.lib)