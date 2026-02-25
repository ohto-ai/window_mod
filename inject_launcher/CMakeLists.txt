cmake_minimum_required(VERSION 3.20)

add_executable(wda_launcher launcher_main.cpp)

target_compile_definitions(wda_launcher PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
)

target_compile_features(wda_launcher PRIVATE cxx_std_17)

# Console subsystem (no WinMain / wWinMain needed).
# MinGW: -municode for wmain entry point.
if (MSVC)
    set_target_properties(wda_launcher PROPERTIES
        LINK_FLAGS "/SUBSYSTEM:CONSOLE"
    )
elseif (MINGW)
    target_link_options(wda_launcher PRIVATE -municode -mconsole)
endif()

target_link_libraries(wda_launcher PRIVATE user32 kernel32 psapi)
