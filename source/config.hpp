#pragma once

#include <functional>
#include <string>

namespace spr {

    inline constexpr const char* CONFIG_DIR   = "sdmc:/switch/spotify-remote";
    inline constexpr const char* CONFIG_PATH  = "sdmc:/switch/spotify-remote/config.ini";

    // Must be registered as a Redirect URI in the Spotify developer dashboard.
    // Shared with tools/spotify_auth.py so both setup paths work with the same app.
    inline constexpr const char* REDIRECT_URI = "http://127.0.0.1:8888/callback";

    inline constexpr const char* SCOPES =
        "user-read-playback-state user-modify-playback-state user-read-currently-playing "
        "playlist-read-private playlist-read-collaborative "
        "user-library-read user-library-modify";

    struct Config {
        std::string clientId;
        std::string clientSecret;
        std::string refreshToken;
        std::string deviceId;      // last device used, for resuming when nothing is active
        int pollMs     = 2000;     // player refresh interval while the overlay is visible
        bool sslVerify = true;
        bool debugLog  = false;    // also write the debug log to debug.log on the SD card

        bool hasCredentials() const { return !clientId.empty() && !clientSecret.empty(); }
        bool isAuthorized() const   { return hasCredentials() && !refreshToken.empty(); }

        // Missing file = defaults, returns false.
        bool load();
        bool save() const;

        // Serialized read-modify-write of the file on disk. Both the worker
        // and the setup server write the config, so all writes go through here.
        static bool update(const std::function<void(Config&)>& fn);
    };

}
