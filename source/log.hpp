#pragma once

#include <string>
#include <vector>

namespace spr {

    inline constexpr const char* LOG_PATH = "sdmc:/switch/spotify-remote/debug.log";

    // Thread-safe debug log. Lines are kept in a small in-memory ring buffer
    // (shown at <setup address>/log while Setup is open) and, when
    // debug_log=1 in config.ini, appended to LOG_PATH. Never log secrets.
    void logf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

    void logSetFileEnabled(bool enabled);
    std::vector<std::string> logLines();
    void logClear();

    // "abcd…wxyz" style shortening for ids/tokens that are useful to recognize but not to leak.
    std::string redact(const std::string& s);

}
