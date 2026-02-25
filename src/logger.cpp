#include "logger.h"

#include <windows.h>
#include <filesystem>
#include <string>
#include <memory>
#include <vector>

#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/msvc_sink.h>

void InitLogger()
{
    // Determine the log-file path: same directory as the running executable.
    wchar_t exeBuf[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exeBuf, MAX_PATH);

    std::filesystem::path logPath =
        std::filesystem::path(exeBuf).parent_path() / L"window_mod.log";

    // Convert to UTF-8 (spdlog filename_t is std::string unless SPDLOG_WCHAR_FILENAMES).
    std::string logPathA;
    {
        int sz = WideCharToMultiByte(CP_UTF8, 0,
                                     logPath.wstring().c_str(), -1,
                                     nullptr, 0, nullptr, nullptr);
        if (sz > 1) {
            logPathA.resize(static_cast<size_t>(sz) - 1);
            WideCharToMultiByte(CP_UTF8, 0,
                                logPath.wstring().c_str(), -1,
                                &logPathA[0], sz, nullptr, nullptr);
        }
    }

    try {
        auto fileSink  = std::make_shared<spdlog::sinks::basic_file_sink_mt>(logPathA, /*truncate=*/true);
        auto debugSink = std::make_shared<spdlog::sinks::msvc_sink_mt>();

        auto logger = std::make_shared<spdlog::logger>(
            "window_mod",
            spdlog::sinks_init_list{ fileSink, debugSink });

        logger->set_level(spdlog::level::debug);
        logger->flush_on(spdlog::level::debug);
        spdlog::set_default_logger(logger);

        spdlog::info("window_mod logger started. Log file: {}", logPathA);
    } catch (const spdlog::spdlog_ex& ex) {
        // Fallback: at least write to the debugger output.
        OutputDebugStringA("window_mod: failed to initialise spdlog file logger: ");
        OutputDebugStringA(ex.what());
        OutputDebugStringA("\n");
    }
}
