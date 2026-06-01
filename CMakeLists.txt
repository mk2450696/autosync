cmake_minimum_required(VERSION 3.15)
project(AutoPacer CXX)

# Static runtime - zero external DLL dependencies
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

find_package(minhook CONFIG REQUIRED)

add_library(AutoPacer SHARED dllmain.cpp)

target_link_libraries(AutoPacer PRIVATE
    minhook::minhook
    d3d11
    dxgi
    user32
    winmm
)

target_compile_features(AutoPacer PRIVATE cxx_std_14)
target_compile_options(AutoPacer PRIVATE /O2 /GL /W3)
target_link_options(AutoPacer PRIVATE /LTCG)