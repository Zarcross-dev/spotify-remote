#include "spotify.hpp"
#include "log.hpp"
#include "util.hpp"

namespace spr {

    // Overridable so the logic can be exercised against a local mock server.
#ifndef SPOTIFY_API_BASE
#define SPOTIFY_API_BASE "https://api.spotify.com/v1"
#endif
#ifndef SPOTIFY_TOKEN_URL
#define SPOTIFY_TOKEN_URL "https://accounts.spotify.com/api/token"
#endif
    static constexpr const char* API_BASE  = SPOTIFY_API_BASE;
    static constexpr const char* TOKEN_URL = SPOTIFY_TOKEN_URL;

    static void parseTrack(const cJSON* item, TrackInfo& t) {
        t = TrackInfo{};
        if (!item || !cJSON_IsObject(item)) return;
        t.uri        = jstr(item, "uri");
        t.name       = jstr(item, "name");
        t.durationMs = static_cast<u32>(jnum(item, "duration_ms"));
        t.isEpisode  = jstr(item, "type") == "episode";
        if (t.isEpisode) {
            const cJSON* show = jget(item, "show");
            t.artists = jstr(show, "name");
            t.album   = jstr(show, "publisher");
        } else {
            t.artists = joinArtists(jget(item, "artists"));
            t.album   = jstr(jget(item, "album"), "name");
        }
    }

    static void parseDevice(const cJSON* d, DeviceInfo& out) {
        out = DeviceInfo{};
        if (!d) return;
        out.id             = jstr(d, "id");
        out.name           = jstr(d, "name");
        out.type           = jstr(d, "type");
        out.active         = jbool(d, "is_active");
        out.restricted     = jbool(d, "is_restricted");
        out.supportsVolume = jbool(d, "supports_volume", true);
        const cJSON* vol   = jget(d, "volume_percent");
        out.volume         = (vol && cJSON_IsNumber(vol)) ? static_cast<int>(vol->valuedouble) : -1;
    }

    void fillApiError(ApiResult& res) {
        if (res.ok) return;
        if (res.status == 0) {
            if (res.error.empty()) res.error = "Network error";
            return;
        }

        Json json(res.body);
        const cJSON* err = jget(json.root(), "error");
        if (err && cJSON_IsObject(err)) {
            res.reason = jstr(err, "reason");
            res.error  = jstr(err, "message");
        } else if (err && cJSON_IsString(err)) {
            // OAuth endpoint style: {"error":"invalid_grant","error_description":"..."}
            res.reason = err->valuestring;
            res.error  = jstr(json.root(), "error_description", err->valuestring);
        }

        if (res.reason == "NO_ACTIVE_DEVICE" || (res.status == 404 && res.error.find("device") != std::string::npos))
            res.error = "No active device - pick one in Devices";
        else if (res.reason == "PREMIUM_REQUIRED")
            res.error = "Spotify Premium is required";
        else if (res.status == 429)
            res.error = "Rate limited by Spotify, slowing down";
        else if (res.status == 403 && res.error.empty())
            res.error = "Not allowed (403)";
        else if (res.error.empty())
            res.error = "HTTP " + std::to_string(res.status);
    }

    void SpotifyApi::configure(const Config& cfg) {
        const bool credsChanged = cfg.clientId != m_cfg.clientId ||
                                  cfg.clientSecret != m_cfg.clientSecret ||
                                  cfg.refreshToken != m_cfg.refreshToken;
        m_cfg = cfg;
        m_http.setVerifyPeer(cfg.sslVerify);
        if (credsChanged) {
            m_accessToken.clear();
            m_tokenExpiresMs = 0;
        }
    }

    bool SpotifyApi::takeRotatedRefreshToken(std::string& out) {
        if (m_rotatedRefreshToken.empty()) return false;
        out = std::move(m_rotatedRefreshToken);
        m_rotatedRefreshToken.clear();
        return true;
    }

    bool SpotifyApi::refreshAccessToken(std::string& error) {
        if (!m_cfg.isAuthorized()) {
            error = "Not set up yet";
            return false;
        }

        const std::string body = "grant_type=refresh_token&refresh_token=" + urlEncode(m_cfg.refreshToken);
        const HttpResponse r = m_http.request("POST", TOKEN_URL, {
            "Authorization: Basic " + base64Encode(m_cfg.clientId + ":" + m_cfg.clientSecret),
            "Content-Type: application/x-www-form-urlencoded",
        }, body);

        if (!r.ok()) {
            ApiResult res;
            res.status = r.status;
            res.body = r.body;
            res.error = r.error;
            fillApiError(res);
            logf("token refresh FAILED: HTTP %ld %s (%s)", r.status, res.reason.c_str(), res.error.c_str());
            if (res.reason == "invalid_grant")
                error = "Login expired - run setup again";
            else if (res.reason == "invalid_client")
                error = "Wrong client id/secret";
            else
                error = "Token refresh failed: " + res.error;
            return false;
        }

        Json json(r.body);
        m_accessToken = jstr(json.root(), "access_token");
        const long long expiresIn = jnum(json.root(), "expires_in", 3600);
        // Renew a minute early so a request never races the expiry.
        m_tokenExpiresMs = nowMs() + static_cast<u64>(expiresIn > 120 ? expiresIn - 60 : expiresIn) * 1000ULL;

        logf("token refreshed, expires in %llds", expiresIn);
        const std::string rotated = jstr(json.root(), "refresh_token");
        if (!rotated.empty() && rotated != m_cfg.refreshToken) {
            m_cfg.refreshToken = rotated;
            m_rotatedRefreshToken = rotated;
            logf("refresh token rotated by Spotify, saving new one");
        }

        if (m_accessToken.empty()) {
            error = "Token refresh returned no token";
            return false;
        }
        return true;
    }

    ApiResult SpotifyApi::call(const char* method, const std::string& path, const std::string& jsonBody) {
        ApiResult res;

        for (int attempt = 0; attempt < 2; ++attempt) {
            if (m_accessToken.empty() || nowMs() >= m_tokenExpiresMs) {
                if (!refreshAccessToken(res.error)) return res;
            }

            std::vector<std::string> headers = { "Authorization: Bearer " + m_accessToken };
            if (!jsonBody.empty()) headers.emplace_back("Content-Type: application/json");

            const u64 start = nowMs();
            const HttpResponse r = m_http.request(method, std::string(API_BASE) + path, headers, jsonBody);
            const unsigned long elapsed = static_cast<unsigned long>(nowMs() - start);
            if (!r.transportOk())
                logf("%s %s -> network error: %s (%lums)", method, path.c_str(), r.error.c_str(), elapsed);
            else
                logf("%s %s -> %ld (%lums)", method, path.c_str(), r.status, elapsed);
            res.status = r.status;
            res.retryAfterSec = r.retryAfterSec;
            res.body = r.body;
            res.error = r.error;
            res.ok = r.ok();

            if (r.status == 401 && attempt == 0) {
                logf("access token rejected, refreshing and retrying");
                m_accessToken.clear();   // expired or revoked, refresh and retry once
                continue;
            }
            break;
        }

        fillApiError(res);
        if (!res.ok && res.status >= 400)
            logf("  error: %s%s%s", res.reason.c_str(), res.reason.empty() ? "" : " - ", res.error.c_str());
        return res;
    }

    bool SpotifyApi::getPlayer(PlayerState& out, ApiResult& res) {
        res = call("GET", "/me/player?additional_types=episode");
        if (!res.ok) return false;

        out = PlayerState{};
        if (res.status == 204 || res.body.empty()) return true;  // nothing playing anywhere

        Json json(res.body);
        const cJSON* root = json.root();
        if (!root) { res.ok = false; res.error = "Bad player response"; return false; }

        out.hasPlayback = true;
        out.playing     = jbool(root, "is_playing");
        out.shuffle     = jbool(root, "shuffle_state");
        out.progressMs  = static_cast<u32>(jnum(root, "progress_ms"));

        const std::string rep = jstr(root, "repeat_state", "off");
        out.repeat = rep == "track" ? Repeat::Track : rep == "context" ? Repeat::Context : Repeat::Off;

        parseDevice(jget(root, "device"), out.device);
        parseTrack(jget(root, "item"), out.track);
        out.contextUri = jstr(jget(root, "context"), "uri");
        return true;
    }

    bool SpotifyApi::getDevices(std::vector<DeviceInfo>& out, ApiResult& res) {
        res = call("GET", "/me/player/devices");
        if (!res.ok) return false;

        out.clear();
        Json json(res.body);
        const cJSON* d = nullptr;
        cJSON_ArrayForEach(d, jget(json.root(), "devices")) {
            DeviceInfo info;
            parseDevice(d, info);
            if (!info.id.empty()) out.push_back(std::move(info));
        }
        return true;
    }

    bool SpotifyApi::getQueue(TrackInfo& current, std::vector<TrackInfo>& out, ApiResult& res) {
        res = call("GET", "/me/player/queue");
        if (!res.ok) return false;

        out.clear();
        Json json(res.body);
        parseTrack(jget(json.root(), "currently_playing"), current);
        const cJSON* item = nullptr;
        cJSON_ArrayForEach(item, jget(json.root(), "queue")) {
            TrackInfo t;
            parseTrack(item, t);
            if (!t.uri.empty()) out.push_back(std::move(t));
        }
        return true;
    }

    bool SpotifyApi::getPlaylists(std::vector<PlaylistInfo>& out, ApiResult& res, size_t maxItems) {
        out.clear();
        // Small pages keep the parsed JSON tree small; the overlay heap is tight.
        for (size_t offset = 0; offset < maxItems; offset += 25) {
            res = call("GET", "/me/playlists?limit=25&offset=" + std::to_string(offset));
            if (!res.ok) return !out.empty();

            Json json(res.body);
            const cJSON* p = nullptr;
            size_t count = 0;
            cJSON_ArrayForEach(p, jget(json.root(), "items")) {
                ++count;
                if (!cJSON_IsObject(p)) continue;
                PlaylistInfo info;
                info.id    = jstr(p, "id");
                info.uri   = jstr(p, "uri");
                info.name  = jstr(p, "name");
                info.owner = jstr(jget(p, "owner"), "display_name");
                // Feb 2026 API renamed playlist.tracks -> playlist.items.
                const cJSON* items = jget(p, "items");
                if (!items) items = jget(p, "tracks");
                info.total = static_cast<u32>(jnum(items, "total"));
                if (!info.uri.empty()) out.push_back(std::move(info));
            }

            const cJSON* next = jget(json.root(), "next");
            if (count == 0 || !next || cJSON_IsNull(next)) break;
        }
        return true;
    }

    bool SpotifyApi::getPlaylistTracks(const std::string& playlistId, std::vector<TrackInfo>& out,
                                       ApiResult& res, size_t maxItems) {
        out.clear();
        for (size_t offset = 0; offset < maxItems; offset += 25) {
            res = call("GET", "/playlists/" + urlEncode(playlistId) + "/items?limit=25&additional_types=episode&offset=" +
                              std::to_string(offset));
            if (!res.ok) {
                if (res.status == 403 && out.empty())
                    res.error = "Spotify only lists tracks of playlists you own";
                return !out.empty();
            }

            Json json(res.body);
            const cJSON* entry = nullptr;
            size_t count = 0;
            cJSON_ArrayForEach(entry, jget(json.root(), "items")) {
                ++count;
                // New field name is "item", older responses use "track".
                const cJSON* obj = jget(entry, "item");
                if (!obj || !cJSON_IsObject(obj)) obj = jget(entry, "track");
                TrackInfo t;
                parseTrack(obj, t);
                if (!t.uri.empty()) out.push_back(std::move(t));
            }

            const cJSON* next = jget(json.root(), "next");
            if (count == 0 || !next || cJSON_IsNull(next)) break;
        }
        return true;
    }

    bool SpotifyApi::isSaved(const std::string& uri, bool& saved, ApiResult& res) {
        res = call("GET", "/me/library/contains?uris=" + urlEncode(uri));
        if (!res.ok) return false;
        Json json(res.body);
        const cJSON* first = cJSON_IsArray(json.root()) ? cJSON_GetArrayItem(json.root(), 0) : nullptr;
        saved = first && cJSON_IsTrue(first);
        return true;
    }

    bool SpotifyApi::exchangeCode(Http& http, const std::string& clientId, const std::string& clientSecret,
                                  const std::string& code, std::string& refreshToken, std::string& error) {
        const std::string body = "grant_type=authorization_code&code=" + urlEncode(code) +
                                 "&redirect_uri=" + urlEncode(REDIRECT_URI);
        const HttpResponse r = http.request("POST", TOKEN_URL, {
            "Authorization: Basic " + base64Encode(clientId + ":" + clientSecret),
            "Content-Type: application/x-www-form-urlencoded",
        }, body);

        if (!r.ok()) {
            ApiResult res;
            res.status = r.status;
            res.body = r.body;
            res.error = r.error;
            fillApiError(res);
            error = res.error;
            logf("code exchange FAILED: HTTP %ld %s (%s)", r.status, res.reason.c_str(), res.error.c_str());
            return false;
        }
        logf("code exchange OK (HTTP %ld)", r.status);

        Json json(r.body);
        refreshToken = jstr(json.root(), "refresh_token");
        if (refreshToken.empty()) {
            error = "Spotify did not return a refresh token";
            return false;
        }
        return true;
    }

}
