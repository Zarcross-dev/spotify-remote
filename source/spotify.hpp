#pragma once

#include "config.hpp"
#include "http.hpp"

#include <switch.h>
#include <string>
#include <vector>

namespace spr {

    struct TrackInfo {
        std::string uri;
        std::string name;
        std::string artists;   // or show name for podcast episodes
        std::string album;
        u32 durationMs = 0;
        bool isEpisode = false;
    };

    struct DeviceInfo {
        std::string id;
        std::string name;
        std::string type;
        int volume = -1;           // -1 when unknown
        bool active = false;
        bool restricted = false;
        bool supportsVolume = true;
    };

    enum class Repeat : u8 { Off, Context, Track };

    struct PlayerState {
        bool hasPlayback = false;  // false => 204, nothing is playing anywhere
        bool playing = false;
        bool shuffle = false;
        Repeat repeat = Repeat::Off;
        u32 progressMs = 0;
        TrackInfo track;
        DeviceInfo device;
        std::string contextUri;
    };

    struct PlaylistInfo {
        std::string id;
        std::string uri;
        std::string name;
        std::string owner;
        u32 total = 0;
    };

    struct ApiResult {
        bool ok = false;
        long status = 0;
        int retryAfterSec = 0;
        std::string reason;    // Spotify's error.reason, e.g. NO_ACTIVE_DEVICE
        std::string error;     // human readable
        std::string body;
    };

    // Thin, synchronous Spotify Web API client. Not thread safe: each thread
    // that talks to Spotify owns its own instance.
    class SpotifyApi {
    public:
        void configure(const Config& cfg);
        void shutdown() { m_http.close(); }

        // Calls https://api.spotify.com/v1<path>, refreshing the access token
        // once on 401. `jsonBody` is sent with Content-Type: application/json.
        ApiResult call(const char* method, const std::string& path, const std::string& jsonBody = {});

        bool getPlayer(PlayerState& out, ApiResult& res);
        bool getDevices(std::vector<DeviceInfo>& out, ApiResult& res);
        bool getQueue(TrackInfo& current, std::vector<TrackInfo>& out, ApiResult& res);
        bool getPlaylists(std::vector<PlaylistInfo>& out, ApiResult& res, size_t maxItems = 200);
        bool getPlaylistTracks(const std::string& playlistId, std::vector<TrackInfo>& out, ApiResult& res, size_t maxItems = 200);
        bool isSaved(const std::string& uri, bool& saved, ApiResult& res);

        // Set when the token endpoint handed back a new refresh token that must be persisted.
        bool takeRotatedRefreshToken(std::string& out);

        // Authorization-code exchange used by the on-Switch setup page.
        static bool exchangeCode(Http& http, const std::string& clientId, const std::string& clientSecret,
                                 const std::string& code, std::string& refreshToken, std::string& error);

    private:
        bool refreshAccessToken(std::string& error);

        Http m_http;
        Config m_cfg;
        std::string m_accessToken;
        u64 m_tokenExpiresMs = 0;
        std::string m_rotatedRefreshToken;
    };

    // Turns a Spotify error body into ApiResult::reason/error.
    void fillApiError(ApiResult& res);

}
