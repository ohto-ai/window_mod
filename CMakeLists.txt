cmake_minimum_required(VERSION 3.20)
project(window_mod LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── Main GUI executable ──────────────────────────────────────────────────────
add_subdirectory(src)

# ── Injection DLL ────────────────────────────────────────────────────────────
add_subdirectory(inject_dll)
