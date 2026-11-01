#include "log.hpp"

#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <deque>
#include <mutex>
#include <sys/stat.h>

namespace spr {

    static constexpr size_t MAX_LINES     = 300;
    static constexpr long MAX_FILE_BYTES  = 256 * 1024;

    static std::mutex s_mutex;
    static std::deque<std::string> s_lines;
    static bool s_fileEnabled = false;

    static void appendToFile(const std::string& line) {
        struct stat st;
        if (stat(LOG_PATH, &st) == 0 && st.st_size > MAX_FILE_BYTES) {
            const std::string old = std::string(LOG_PATH) + ".old";
            remove(old.c_str());
            rename(LOG_PATH, old.c_str());
        }
        if (FILE* f = fopen(LOG_PATH, "a")) {
            fputs(line.c_str(), f);
            fputc('\n', f);
            fclose(f);
        }
    }

    void logf(const char* fmt, ...) {
        char msg[512];
        va_list args;
        va_start(args, fmt);
        vsnprintf(msg, sizeof(msg), fmt, args);
        va_end(args);

        char stamp[16] = "--:--:--";
        const time_t now = time(nullptr);
        tm local{};
        if (localtime_r(&now, &local)) strftime(stamp, sizeof(stamp), "%H:%M:%S", &local);

        std::string line = std::string(stamp) + "  " + msg;

        std::lock_guard lock(s_mutex);
        if (s_fileEnabled) appendToFile(line);
        s_lines.push_back(std::move(line));
        if (s_lines.size() > MAX_LINES) s_lines.pop_front();
    }

    void logSetFileEnabled(bool enabled) {
        std::lock_guard lock(s_mutex);
        s_fileEnabled = enabled;
    }

    std::vector<std::string> logLines() {
        std::lock_guard lock(s_mutex);
        return {s_lines.begin(), s_lines.end()};
    }

    void logClear() {
        std::lock_guard lock(s_mutex);
        s_lines.clear();
    }

    std::string redact(const std::string& s) {
        if (s.empty()) return "(empty)";
        if (s.size() <= 8) return "****";
        return s.substr(0, 4) + "..." + s.substr(s.size() - 4);
    }

}
