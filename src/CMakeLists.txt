cmake_minimum_required(VERSION 3.20)

set(SRC
    main.cpp
    window_list.cpp
    window_ops.cpp
    injector.cpp
    logger.cpp
    window_mod.rc
)

add_executable(window_mod WIN32 ${SRC})

target_compile_definitions(window_mod PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

target_compile_features(window_mod PRIVATE cxx_std_17)

# MinGW needs -municode so that the CRT startup code calls wWinMain.
if (MINGW)
    target_link_options(window_mod PRIVATE -municode)
endif()

target_link_libraries(window_mod PRIVATE
    user32
    gdi32
    psapi
    comctl32
    shell32
    dwmapi
    uxtheme
    spdlog::spdlog
)

# Copy the injection DLL next to the exe after build.
add_custom_command(TARGET window_mod POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:wda_inject>"
        "$<TARGET_FILE_DIR:window_mod>/wda_inject.dll"
    COMMENT "Copying wda_inject.dll to output directory"
)

# Copy the same-arch launcher next to the exe after build.
# The opposite-arch launcher is copied by the CI packaging step.
add_custom_command(TARGET window_mod POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:wda_launcher>"
        "$<TARGET_FILE_DIR:window_mod>/$<IF:$<EQUAL:${CMAKE_SIZEOF_VOID_P},8>,wda_launcher_x64.exe,wda_launcher_x86.exe>"
    COMMENT "Copying wda_launcher to output directory"
)
