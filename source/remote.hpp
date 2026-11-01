#pragma once

#include "spotify.hpp"

#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace spr {

    enum class Cmd : u8 {
        Refresh,
        ReloadConfig,
        SetPlaying,         // arg: 1 play, 0 pause
        Next,
        Previous,
        SetShuffle,         // arg: 0/1
        SetRepeat,          // arg: Repeat
        SetLike,            // arg: 0/1, a: track uri
        Transfer,           // a: device id
        PlayContext,        // a: context uri, b: optional track uri to start from
        PlayTrack,          // a: track uri (no context)
        SkipForward,        // arg: number of tracks to skip
        LoadDevices,
        LoadPlaylists,
        LoadPlaylistTracks, // a: playlist id
        LoadQueue,
    };

    struct Command {
        Cmd type;
        int arg = 0;
        std::string a, b;
    };

    // What the UI renders. Copied out of the worker under a lock.
    struct Snapshot {
        bool configured = false;   // client id + secret present
        bool authorized = false;   // refresh token present
        bool loaded = false;       // at least one player fetch finished
        bool connected = false;    // last player fetch succeeded
        PlayerState player;
        u64 progressAtMs = 0;      // nowMs() at which player.progressMs was exact
        bool likedKnown = false;
        bool liked = false;
        std::string message;
        bool messageIsError = false;
        u64 messageAtMs = 0;
        u32 version = 0;

        // Interpolated playback position for smooth progress bars.
        u32 currentProgressMs() const;
    };

    template<typename T>
    struct AsyncList {
        std::vector<T> items;
        std::string key;       // e.g. playlist id the items belong to
        std::string error;
        bool loading = false;
        u32 version = 0;
    };

    // Background worker that owns all network traffic. The UI posts commands
    // and reads snapshots; it never blocks on the network.
    class Remote {
    public:
        static Remote& get();

        void start();
        void stop();

        void setVisible(bool visible);
        void post(Command cmd);
        void setVolume(int percent);    // debounced, latest value wins
        void seekTo(u32 positionMs);    // debounced, latest value wins

        bool snapshotIfChanged(u32& seenVersion, Snapshot& out);
        Snapshot snapshot();

        bool devicesIfChanged(u32& seen, AsyncList<DeviceInfo>& out);
        bool playlistsIfChanged(u32& seen, AsyncList<PlaylistInfo>& out);
        bool playlistTracksIfChanged(u32& seen, AsyncList<TrackInfo>& out);
        // items[0] is the currently playing track, the rest is the upcoming queue.
        bool queueIfChanged(u32& seen, AsyncList<TrackInfo>& out);

    private:
        Remote() = default;

        static void threadEntry(void* arg);
        void threadMain();

        void execute(const Command& cmd);
        void pollPlayer();
        void reloadConfig();
        void sendVolume(int percent);
        void sendSeek(u32 positionMs);
        ApiResult resume(const std::string& jsonBody);
        std::string pickFallbackDevice();
        void afterApiCall(const ApiResult& res);
        void persistTokenRotation();

        void setMessage(const std::string& msg, bool isError);
        void setMessageLocked(const std::string& msg, bool isError);
        void reportResult(const ApiResult& res);

        // ---- shared, guarded by m_mutex ----
        std::mutex m_mutex;
        std::condition_variable m_cv;
        std::deque<Command> m_queue;
        Snapshot m_snap;
        AsyncList<DeviceInfo> m_devices;
        AsyncList<PlaylistInfo> m_playlists;
        AsyncList<TrackInfo> m_playlistTracks;
        AsyncList<TrackInfo> m_queueList;
        bool m_visible = true;
        bool m_stopRequested = false;
        bool m_pollNow = true;
        int m_pendingVolume = -1;
        s64 m_pendingSeek = -1;
        u64 m_lastFlushMs = 0;
        // Until these times, polled values don't overwrite optimistic UI state
        // (Spotify often reports the old state for a moment after a command).
        u64 m_holdPlayingUntil = 0, m_holdShuffleUntil = 0, m_holdRepeatUntil = 0;
        u64 m_holdVolumeUntil = 0, m_holdProgressUntil = 0;

        // ---- worker thread only ----
        SpotifyApi m_api;
        Config m_cfg;
        u64 m_nextPollMs = 0;
        std::string m_likedUri;   // track whose liked state is in m_snap

        Thread m_thread{};
        bool m_running = false;
    };

}
