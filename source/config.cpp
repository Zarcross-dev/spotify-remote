#include "config.hpp"
#include "util.hpp"

#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <sys/stat.h>

namespace spr {

    static std::mutex s_fileMutex;

    static std::string unquote(std::string v) {
        if (v.size() >= 2 && ((v.front() == '"' && v.back() == '"') || (v.front() == '\'' && v.back() == '\'')))
            return v.substr(1, v.size() - 2);
        return v;
    }

    // Values are written one per line, so a newline inside one would let it
    // inject other keys. Strip control characters on the way out.
    static std::string clean(const std::string& v) {
        std::string out;
        for (unsigned char c : v)
            if (c >= 0x20 && c != 0x7F) out += static_cast<char>(c);
        return out;
    }

    static bool loadLocked(Config& cfg) {
        FILE* f = fopen(CONFIG_PATH, "r");
        // A save interrupted between remove() and rename() leaves only the temp file.
        if (!f) f = fopen((std::string(CONFIG_PATH) + ".tmp").c_str(), "r");
        if (!f) return false;

        char line[1024];
        while (fgets(line, sizeof(line), f)) {
            const std::string l = trim(line);
            if (l.empty() || l[0] == '#' || l[0] == ';' || l[0] == '[') continue;
            const size_t eq = l.find('=');
            if (eq == std::string::npos) continue;

            const std::string key = trim(l.substr(0, eq));
            const std::string val = unquote(trim(l.substr(eq + 1)));

            if (key == "client_id")          cfg.clientId = val;
            else if (key == "client_secret") cfg.clientSecret = val;
            else if (key == "refresh_token") cfg.refreshToken = val;
            else if (key == "device_id")     cfg.deviceId = val;
            else if (key == "poll_interval_ms") {
                const int v = atoi(val.c_str());
                if (v >= 500 && v <= 60000) cfg.pollMs = v;
            }
            else if (key == "ssl_verify")    cfg.sslVerify = !(val == "0" || val == "false" || val == "no");
            else if (key == "debug_log")     cfg.debugLog = val == "1" || val == "true" || val == "yes";
        }
        fclose(f);
        return true;
    }

    static bool saveLocked(const Config& cfg) {
        mkdir("sdmc:/switch", 0777);
        mkdir(CONFIG_DIR, 0777);

        const std::string tmpPath = std::string(CONFIG_PATH) + ".tmp";
        FILE* f = fopen(tmpPath.c_str(), "w");
        if (!f) return false;

        fprintf(f,
            "; Spotify Remote overlay configuration\n"
            "; Create an app at https://developer.spotify.com/dashboard and add this Redirect URI:\n"
            ";   %s\n"
            "; refresh_token is filled in by the on-Switch setup page or tools/spotify_auth.py.\n"
            "[spotify]\n"
            "client_id=%s\n"
            "client_secret=%s\n"
            "refresh_token=%s\n"
            "device_id=%s\n"
            "poll_interval_ms=%d\n"
            "ssl_verify=%d\n"
            "debug_log=%d\n",
            REDIRECT_URI,
            clean(cfg.clientId).c_str(), clean(cfg.clientSecret).c_str(), clean(cfg.refreshToken).c_str(),
            clean(cfg.deviceId).c_str(), cfg.pollMs, cfg.sslVerify ? 1 : 0, cfg.debugLog ? 1 : 0);

        const bool ok = fflush(f) == 0;
        fclose(f);
        if (!ok) { remove(tmpPath.c_str()); return false; }

        // FAT has no atomic replace; remove first so rename succeeds.
        remove(CONFIG_PATH);
        return rename(tmpPath.c_str(), CONFIG_PATH) == 0;
    }

    bool Config::load() {
        std::lock_guard lock(s_fileMutex);
        *this = Config{};
        return loadLocked(*this);
    }

    bool Config::save() const {
        std::lock_guard lock(s_fileMutex);
        return saveLocked(*this);
    }

    bool Config::update(const std::function<void(Config&)>& fn) {
        std::lock_guard lock(s_fileMutex);
        Config cfg;
        loadLocked(cfg);
        fn(cfg);
        return saveLocked(cfg);
    }

}
