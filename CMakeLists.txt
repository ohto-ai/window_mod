cmake_minimum_required(VERSION 3.20)
project(window_mod LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── spdlog ───────────────────────────────────────────────────────────────────
include(FetchContent)
set(SPDLOG_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
FetchContent_Declare(
    spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG        v1.15.1
)
FetchContent_MakeAvailable(spdlog)

# ── Main GUI executable ──────────────────────────────────────────────────────
add_subdirectory(src)

# ── Injection DLL ────────────────────────────────────────────────────────────
add_subdirectory(inject_dll)

# ── Injection launcher helper (same-arch CLI injector) ───────────────────────
add_subdirectory(inject_launcher)
