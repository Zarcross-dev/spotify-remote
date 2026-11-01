#include "remote.hpp"
#include "log.hpp"
#include "util.hpp"

#include <algorithm>
#include <chrono>

namespace spr {

    static constexpr u64 HOLD_MS          = 2500;  // keep optimistic state this long
    static constexpr u64 AFTER_CMD_POLL   = 700;   // re-sync this soon after a command
    static constexpr u64 ERROR_BACKOFF_MS = 5000;
    static constexpr u64 DEBOUNCE_MS      = 250;   // volume / seek coalescing
    static constexpr size_t THREAD_STACK  = 0x20000;

    u32 Snapshot::currentProgressMs() const {
        u64 p = player.progressMs;
        if (player.playing && progressAtMs)
            p += nowMs() - progressAtMs;
        if (player.track.durationMs && p > player.track.durationMs)
            p = player.track.durationMs;
        return static_cast<u32>(p);
    }

    Remote& Remote::get() {
        static Remote instance;
        return instance;
    }

    void Remote::start() {
        if (m_running) return;
        {
            std::lock_guard lock(m_mutex);
            m_stopRequested = false;
        }
        if (R_SUCCEEDED(threadCreate(&m_thread, &Remote::threadEntry, this, nullptr, THREAD_STACK, 0x2C, -2))) {
            if (R_SUCCEEDED(threadStart(&m_thread)))
                m_running = true;
            else
                threadClose(&m_thread);
        }
    }

    void Remote::stop() {
        if (!m_running) return;
        {
            std::lock_guard lock(m_mutex);
            m_stopRequested = true;
        }
        m_cv.notify_all();
        threadWaitForExit(&m_thread);
        threadClose(&m_thread);
        m_running = false;
    }

    void Remote::threadEntry(void* arg) {
        static_cast<Remote*>(arg)->threadMain();
    }

    void Remote::setVisible(bool visible) {
        {
            std::lock_guard lock(m_mutex);
            m_visible = visible;
            if (visible) m_pollNow = true;
        }
        m_cv.notify_all();
    }

    // ------------------------------------------------------------------
    // UI-facing API
    // ------------------------------------------------------------------

    // Commands where only the newest one matters: an older queued one is
    // dropped, so mashing a button while the network is slow doesn't replay
    // every press once it comes back.
    static bool latestWins(Cmd c) {
        switch (c) {
            case Cmd::Refresh: case Cmd::ReloadConfig: case Cmd::SetPlaying: case Cmd::SetShuffle:
            case Cmd::SetRepeat: case Cmd::SetLike: case Cmd::Transfer: case Cmd::PlayContext:
            case Cmd::PlayTrack: case Cmd::LoadDevices: case Cmd::LoadPlaylists:
            case Cmd::LoadPlaylistTracks: case Cmd::LoadQueue:
                return true;
            default:
                return false;
        }
    }

    static constexpr size_t MAX_QUEUED = 16;

    void Remote::post(Command cmd) {
        {
            std::lock_guard lock(m_mutex);
            const u64 now = nowMs();
            PlayerState& p = m_snap.player;
            bool changed = true;

            // Optimistic updates so the UI reacts on the same frame.
            switch (cmd.type) {
                case Cmd::SetPlaying:
                    p.progressMs = m_snap.currentProgressMs();
                    m_snap.progressAtMs = now;
                    p.playing = cmd.arg != 0;
                    m_holdPlayingUntil = now + HOLD_MS;
                    break;
                case Cmd::SetShuffle:
                    p.shuffle = cmd.arg != 0;
                    m_holdShuffleUntil = now + HOLD_MS;
                    break;
                case Cmd::SetRepeat:
                    p.repeat = static_cast<Repeat>(cmd.arg);
                    m_holdRepeatUntil = now + HOLD_MS;
                    break;
                case Cmd::SetLike:
                    m_snap.liked = cmd.arg != 0;
                    m_snap.likedKnown = true;
                    break;
                case Cmd::LoadDevices:
                    m_devices.loading = true; m_devices.error.clear(); ++m_devices.version;
                    break;
                case Cmd::LoadPlaylists:
                    m_playlists.loading = true; m_playlists.error.clear(); ++m_playlists.version;
                    break;
                case Cmd::LoadPlaylistTracks:
                    if (m_playlistTracks.key != cmd.a) m_playlistTracks.items.clear();
                    m_playlistTracks.key = cmd.a;
                    m_playlistTracks.loading = true; m_playlistTracks.error.clear(); ++m_playlistTracks.version;
                    break;
                case Cmd::LoadQueue:
                    m_queueList.loading = true; m_queueList.error.clear(); ++m_queueList.version;
                    break;
                default:
                    changed = false;
                    break;
            }
            if (latestWins(cmd.type)) {
                std::erase_if(m_queue, [&](const Command& c) { return c.type == cmd.type; });
            } else if (m_queue.size() >= MAX_QUEUED) {
                setMessageLocked("Spotify is not responding, try again", true);
                return;
            }
            if (changed) ++m_snap.version;
            m_queue.push_back(std::move(cmd));
        }
        m_cv.notify_all();
    }

    void Remote::setVolume(int percent) {
        {
            std::lock_guard lock(m_mutex);
            percent = std::clamp(percent, 0, 100);
            m_pendingVolume = percent;
            m_snap.player.device.volume = percent;
            m_holdVolumeUntil = nowMs() + HOLD_MS;
            ++m_snap.version;
        }
        m_cv.notify_all();
    }

    void Remote::seekTo(u32 positionMs) {
        {
            std::lock_guard lock(m_mutex);
            const u32 dur = m_snap.player.track.durationMs;
            if (dur && positionMs > dur - 1000) positionMs = dur > 1000 ? dur - 1000 : 0;
            m_pendingSeek = positionMs;
            m_snap.player.progressMs = positionMs;
            m_snap.progressAtMs = nowMs();
            m_holdProgressUntil = nowMs() + HOLD_MS;
            ++m_snap.version;
        }
        m_cv.notify_all();
    }

    bool Remote::snapshotIfChanged(u32& seenVersion, Snapshot& out) {
        std::lock_guard lock(m_mutex);
        if (m_snap.version == seenVersion) return false;
        out = m_snap;
        seenVersion = m_snap.version;
        return true;
    }

    Snapshot Remote::snapshot() {
        std::lock_guard lock(m_mutex);
        return m_snap;
    }

    template<typename T>
    static bool copyIfChanged(std::mutex& m, const AsyncList<T>& src, u32& seen, AsyncList<T>& out) {
        std::lock_guard lock(m);
        if (src.version == seen) return false;
        out = src;
        seen = src.version;
        return true;
    }

    bool Remote::devicesIfChanged(u32& seen, AsyncList<DeviceInfo>& out)          { return copyIfChanged(m_mutex, m_devices, seen, out); }
    bool Remote::playlistsIfChanged(u32& seen, AsyncList<PlaylistInfo>& out)      { return copyIfChanged(m_mutex, m_playlists, seen, out); }
    bool Remote::playlistTracksIfChanged(u32& seen, AsyncList<TrackInfo>& out)    { return copyIfChanged(m_mutex, m_playlistTracks, seen, out); }
    bool Remote::queueIfChanged(u32& seen, AsyncList<TrackInfo>& out)             { return copyIfChanged(m_mutex, m_queueList, seen, out); }

    // ------------------------------------------------------------------
    // Worker
    // ------------------------------------------------------------------

    void Remote::setMessageLocked(const std::string& msg, bool isError) {
        m_snap.message = msg;
        m_snap.messageIsError = isError;
        m_snap.messageAtMs = nowMs();
        ++m_snap.version;
    }

    void Remote::setMessage(const std::string& msg, bool isError) {
        std::lock_guard lock(m_mutex);
        setMessageLocked(msg, isError);
    }

    void Remote::reportResult(const ApiResult& res) {
        if (!res.ok) setMessage(res.error, true);
    }

    void Remote::persistTokenRotation() {
        std::string rotated;
        if (m_api.takeRotatedRefreshToken(rotated)) {
            m_cfg.refreshToken = rotated;
            Config::update([&](Config& c) { c.refreshToken = rotated; });
        }
    }

    void Remote::afterApiCall(const ApiResult& res) {
        persistTokenRotation();
        const u64 now = nowMs();
        if (res.status == 429)
            m_nextPollMs = now + std::max<u64>(2000, static_cast<u64>(res.retryAfterSec) * 1000);
        else
            m_nextPollMs = std::min(m_nextPollMs, now + AFTER_CMD_POLL);
    }

    void Remote::reloadConfig() {
        Config cfg;
        cfg.load();
        m_cfg = cfg;
        m_api.configure(cfg);
        logSetFileEnabled(cfg.debugLog);
        logf("config loaded: client_id=%s secret=%s refresh_token=%s device=%s poll=%dms ssl_verify=%d",
             redact(cfg.clientId).c_str(), cfg.clientSecret.empty() ? "missing" : "set",
             cfg.refreshToken.empty() ? "missing" : "set", redact(cfg.deviceId).c_str(), cfg.pollMs, cfg.sslVerify);

        std::lock_guard lock(m_mutex);
        m_snap.configured = cfg.hasCredentials();
        m_snap.authorized = cfg.isAuthorized();
        m_snap.loaded = false;
        m_pollNow = true;
        ++m_snap.version;
    }

    std::string Remote::pickFallbackDevice() {
        std::vector<DeviceInfo> devices;
        ApiResult res;
        if (!m_api.getDevices(devices, res) || devices.empty()) return {};

        for (const auto& d : devices)
            if (d.id == m_cfg.deviceId && !d.restricted) return d.id;
        for (const auto& d : devices)
            if (!d.restricted) return d.id;
        return {};
    }

    // Starts/resumes playback, transferring to the last used (or any) device
    // when Spotify reports that nothing is active.
    ApiResult Remote::resume(const std::string& jsonBody) {
        ApiResult res = m_api.call("PUT", "/me/player/play", jsonBody);
        // Only a missing device triggers the fallback, not e.g. a bad playlist URI.
        if (res.ok || !(res.reason == "NO_ACTIVE_DEVICE" || (res.status == 404 && res.reason.empty()))) return res;

        const std::string deviceId = pickFallbackDevice();
        logf("no active device, falling back to %s", deviceId.empty() ? "(none found)" : redact(deviceId).c_str());
        if (deviceId.empty()) {
            res.error = "No Spotify device found. Open Spotify on a phone, PC or speaker first.";
            return res;
        }

        if (jsonBody.empty())
            res = m_api.call("PUT", "/me/player", "{\"device_ids\":[\"" + jsonEscape(deviceId) + "\"],\"play\":true}");
        else
            res = m_api.call("PUT", "/me/player/play?device_id=" + urlEncode(deviceId), jsonBody);
        return res;
    }

    void Remote::sendVolume(int percent) {
        const ApiResult res = m_api.call("PUT", "/me/player/volume?volume_percent=" + std::to_string(percent));
        if (!res.ok && res.reason == "VOLUME_CONTROL_DISALLOW")
            setMessage("This device doesn't allow remote volume", true);
        else
            reportResult(res);
        afterApiCall(res);
    }

    void Remote::sendSeek(u32 positionMs) {
        const ApiResult res = m_api.call("PUT", "/me/player/seek?position_ms=" + std::to_string(positionMs));
        reportResult(res);
        afterApiCall(res);
    }

    static const char* cmdName(Cmd c) {
        static constexpr const char* names[] = {
            "Refresh", "ReloadConfig", "SetPlaying", "Next", "Previous", "SetShuffle", "SetRepeat",
            "SetLike", "Transfer", "PlayContext", "PlayTrack", "SkipForward", "LoadDevices",
            "LoadPlaylists", "LoadPlaylistTracks", "LoadQueue",
        };
        const size_t i = static_cast<size_t>(c);
        return i < sizeof(names) / sizeof(names[0]) ? names[i] : "?";
    }

    void Remote::execute(const Command& cmd) {
        logf("cmd %s arg=%d%s%s", cmdName(cmd.type), cmd.arg, cmd.a.empty() ? "" : " ", cmd.a.c_str());
        if (cmd.type == Cmd::ReloadConfig) {
            reloadConfig();
            if (m_cfg.isAuthorized()) setMessage("Config loaded", false);
            return;
        }
        if (cmd.type == Cmd::Refresh) {
            std::lock_guard lock(m_mutex);
            m_pollNow = true;
            return;
        }
        if (!m_cfg.isAuthorized()) {
            setMessage("Not set up yet - open Setup", true);
            return;
        }

        ApiResult res;
        switch (cmd.type) {
            case Cmd::SetPlaying:
                res = cmd.arg ? resume({}) : m_api.call("PUT", "/me/player/pause");
                break;

            case Cmd::Next:
                res = m_api.call("POST", "/me/player/next");
                break;

            case Cmd::Previous:
                res = m_api.call("POST", "/me/player/previous");
                break;

            case Cmd::SetShuffle:
                res = m_api.call("PUT", std::string("/me/player/shuffle?state=") + (cmd.arg ? "true" : "false"));
                break;

            case Cmd::SetRepeat: {
                static constexpr const char* names[] = { "off", "context", "track" };
                res = m_api.call("PUT", std::string("/me/player/repeat?state=") + names[std::clamp(cmd.arg, 0, 2)]);
                break;
            }

            case Cmd::SetLike:
                res = m_api.call(cmd.arg ? "PUT" : "DELETE", "/me/library?uris=" + urlEncode(cmd.a));
                if (res.ok) setMessage(cmd.arg ? "Saved to your library" : "Removed from your library", false);
                break;

            case Cmd::Transfer:
                res = m_api.call("PUT", "/me/player", "{\"device_ids\":[\"" + jsonEscape(cmd.a) + "\"],\"play\":true}");
                if (res.ok) {
                    m_cfg.deviceId = cmd.a;
                    Config::update([&](Config& c) { c.deviceId = cmd.a; });
                }
                break;

            case Cmd::PlayContext: {
                std::string body = "{\"context_uri\":\"" + jsonEscape(cmd.a) + "\"";
                if (!cmd.b.empty()) body += ",\"offset\":{\"uri\":\"" + jsonEscape(cmd.b) + "\"}";
                body += "}";
                res = resume(body);
                break;
            }

            case Cmd::PlayTrack:
                res = resume("{\"uris\":[\"" + jsonEscape(cmd.a) + "\"]}");
                break;

            case Cmd::SkipForward:
                for (int i = 0; i < cmd.arg; ++i) {
                    res = m_api.call("POST", "/me/player/next");
                    if (!res.ok) break;
                }
                break;

            case Cmd::LoadDevices: {
                std::vector<DeviceInfo> items;
                const bool ok = m_api.getDevices(items, res);
                std::lock_guard lock(m_mutex);
                m_devices.items = std::move(items);
                m_devices.error = ok ? "" : res.error;
                m_devices.loading = false;
                ++m_devices.version;
                persistTokenRotation();
                return;
            }

            case Cmd::LoadPlaylists: {
                std::vector<PlaylistInfo> items;
                const bool ok = m_api.getPlaylists(items, res);
                std::lock_guard lock(m_mutex);
                m_playlists.items = std::move(items);
                m_playlists.error = ok ? "" : res.error;
                m_playlists.loading = false;
                ++m_playlists.version;
                persistTokenRotation();
                return;
            }

            case Cmd::LoadPlaylistTracks: {
                std::vector<TrackInfo> items;
                const bool ok = m_api.getPlaylistTracks(cmd.a, items, res);
                std::lock_guard lock(m_mutex);
                if (m_playlistTracks.key == cmd.a) {
                    m_playlistTracks.items = std::move(items);
                    m_playlistTracks.error = ok ? "" : res.error;
                    m_playlistTracks.loading = false;
                    ++m_playlistTracks.version;
                }
                persistTokenRotation();
                return;
            }

            case Cmd::LoadQueue: {
                TrackInfo current;
                std::vector<TrackInfo> items;
                const bool ok = m_api.getQueue(current, items, res);
                items.insert(items.begin(), std::move(current));
                std::lock_guard lock(m_mutex);
                m_queueList.items = std::move(items);
                m_queueList.error = ok ? "" : res.error;
                m_queueList.loading = false;
                ++m_queueList.version;
                persistTokenRotation();
                return;
            }

            default:
                return;
        }

        if (!res.ok) {
            // Undo optimistic state by letting the next poll overwrite it right away.
            std::lock_guard lock(m_mutex);
            m_holdPlayingUntil = m_holdShuffleUntil = m_holdRepeatUntil = 0;
            setMessageLocked(res.error, true);
        }
        afterApiCall(res);
    }

    void Remote::pollPlayer() {
        PlayerState fresh;
        ApiResult res;
        const bool ok = m_api.getPlayer(fresh, res);
        persistTokenRotation();
        const u64 now = nowMs();

        // Remember the active device so playback can be resumed on it later.
        if (ok && !fresh.device.id.empty() && fresh.device.id != m_cfg.deviceId) {
            m_cfg.deviceId = fresh.device.id;
            Config::update([&](Config& c) { c.deviceId = fresh.device.id; });
        }

        // The liked state is per track, so only ask when the track changes.
        bool likedKnown = false, liked = false;
        const bool trackChanged = ok && fresh.track.uri != m_likedUri;
        if (trackChanged && !fresh.track.uri.empty()) {
            ApiResult likeRes;
            likedKnown = m_api.isSaved(fresh.track.uri, liked, likeRes);
        }

        {
            std::lock_guard lock(m_mutex);
            m_snap.loaded = true;
            m_snap.connected = ok;

            if (ok) {
                PlayerState& cur = m_snap.player;
                const bool sameTrack = cur.track.uri == fresh.track.uri;
                if (now < m_holdPlayingUntil && sameTrack) fresh.playing = cur.playing;
                if (now < m_holdShuffleUntil) fresh.shuffle = cur.shuffle;
                if (now < m_holdRepeatUntil)  fresh.repeat = cur.repeat;
                if (now < m_holdVolumeUntil)  fresh.device.volume = cur.device.volume;

                if (now < m_holdProgressUntil && sameTrack) {
                    fresh.progressMs = cur.progressMs;
                } else {
                    m_snap.progressAtMs = now;
                }
                cur = std::move(fresh);

                if (trackChanged) {
                    m_likedUri = cur.track.uri;
                    m_snap.likedKnown = likedKnown;
                    m_snap.liked = liked;
                }

                // A successful poll clears stale network errors.
                if (m_snap.messageIsError && now - m_snap.messageAtMs > 3000) m_snap.message.clear();
            } else {
                setMessageLocked(res.error, true);
            }
            ++m_snap.version;
        }

        // Schedule the next poll: sooner near the end of a track so the
        // next song shows up promptly, slower after errors.
        u64 delay = static_cast<u64>(m_cfg.pollMs);
        if (!ok) {
            delay = res.status == 429 ? std::max<u64>(2000, static_cast<u64>(res.retryAfterSec) * 1000) : ERROR_BACKOFF_MS;
        } else {
            const Snapshot s = snapshot();
            if (s.player.playing && s.player.track.durationMs) {
                const u32 pos = s.currentProgressMs();
                const u64 remaining = s.player.track.durationMs > pos ? s.player.track.durationMs - pos : 0;
                delay = std::min<u64>(delay, remaining + 400);
            }
        }
        m_nextPollMs = now + std::max<u64>(delay, 300);
    }

    void Remote::threadMain() {
        logf("worker started");
        reloadConfig();

        std::unique_lock lock(m_mutex);
        while (!m_stopRequested) {
            const u64 now = nowMs();
            const bool canPoll = m_visible && m_snap.authorized;
            const bool pollDue = canPoll && (m_pollNow || now >= m_nextPollMs);
            const bool hasPending = m_pendingVolume >= 0 || m_pendingSeek >= 0;
            // Throttle rather than debounce: the first change goes out at once,
            // later ones at most every DEBOUNCE_MS while the user keeps adjusting.
            const bool flushDue = hasPending && now - m_lastFlushMs >= DEBOUNCE_MS;

            if (m_queue.empty() && !pollDue && !flushDue) {
                u64 waitMs = canPoll ? m_nextPollMs - now : 60000;
                if (hasPending) waitMs = std::min<u64>(waitMs, m_lastFlushMs + DEBOUNCE_MS - now);
                m_cv.wait_for(lock, std::chrono::milliseconds(std::max<u64>(waitMs, 1)));
                continue;
            }

            std::deque<Command> cmds;
            cmds.swap(m_queue);
            int volume = -1;
            s64 seek = -1;
            if (flushDue) {
                volume = m_pendingVolume;
                seek = m_pendingSeek;
                m_pendingVolume = -1;
                m_pendingSeek = -1;
                m_lastFlushMs = now;
            }
            m_pollNow = false;
            lock.unlock();

            for (const auto& cmd : cmds) execute(cmd);
            if (m_cfg.isAuthorized()) {
                if (volume >= 0) sendVolume(volume);
                if (seek >= 0) sendSeek(static_cast<u32>(seek));
            }

            lock.lock();
            if (m_visible && m_snap.authorized && (pollDue || m_pollNow || nowMs() >= m_nextPollMs)) {
                m_pollNow = false;
                lock.unlock();
                pollPlayer();
                lock.lock();
            }
        }

        // Release curl/TLS on this thread while the network services are still up.
        lock.unlock();
        m_api.shutdown();
        logf("worker stopped");
    }

}
