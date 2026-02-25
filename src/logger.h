#pragma once

/// Call once at application startup (before any logging).
/// Creates (or truncates) "window_mod.log" next to the executable and registers
/// it as the spdlog default logger.  All subsequent spdlog calls (info / warn /
/// error / debug) write to that file and to the debugger output (OutputDebugString).
void InitLogger();
