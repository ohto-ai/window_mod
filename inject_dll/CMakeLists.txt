cmake_minimum_required(VERSION 3.20)

add_library(wda_inject SHARED
    dllmain.cpp
)

target_compile_definitions(wda_inject PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

target_compile_features(wda_inject PRIVATE cxx_std_17)

target_link_libraries(wda_inject PRIVATE
    user32
)

# Suppress the default 'lib' prefix on MinGW.
set_target_properties(wda_inject PROPERTIES PREFIX "")
