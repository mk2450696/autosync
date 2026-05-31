cmake_minimum_required(VERSION 3.15)
project(AutoPacer)

# Find the MinHook library automatically
find_package(minhook CONFIG REQUIRED)

# Compile as a DLL
add_library(AutoPacer SHARED dllmain.cpp)

# Link DirectX and MinHook
target_link_libraries(AutoPacer PRIVATE minhook::minhook d3d11.lib dxgi.lib user32.lib)