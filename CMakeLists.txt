cmake_minimum_required(VERSION 3.15)
project(AutoPacer CXX)

# Force static MSVC runtime - no external DLL dependencies
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

# vcpkg toolchain handles minhook discovery
find_package(minhook CONFIG REQUIRED)

# Build as a DLL
add_library(AutoPacer SHARED dllmain.cpp)

# Link everything statically
target_link_libraries(AutoPacer PRIVATE
    minhook::minhook
    d3d11
    dxgi
    user32
)

# C++14 minimum for chrono and atomic
target_compile_features(AutoPacer PRIVATE cxx_std_14)

# Release optimizations
target_compile_options(AutoPacer PRIVATE /O2 /GL)
target_link_options(AutoPacer PRIVATE /LTCG)
