#define TESLA_INIT_IMPL
#include <exception_wrap.hpp>
#include <tesla.hpp>

#include "remote.hpp"
#include "setup_server.hpp"
#include "util.hpp"

#include <curl/curl.h>

using namespace spr;

namespace {

    constexpr tsl::Color COLOR_GREEN {0x1, 0xD, 0x6, 0xF};
    constexpr tsl::Color COLOR_TEXT  {0xF, 0xF, 0xF, 0xF};
    constexpr tsl::Color COLOR_SUB   {0xB, 0xB, 0xB, 0xF};
    constexpr tsl::Color COLOR_DIM   {0x7, 0x7, 0x7, 0xF};
    constexpr tsl::Color COLOR_TRACK {0x4, 0x4, 0x4, 0xF};
    constexpr tsl::Color COLOR_ERROR {0xF, 0x6, 0x6, 0xF};

    constexpr u64 MESSAGE_SHOW_MS = 4000;
    constexpr u32 SEEK_STEP_MS    = 15000;
    constexpr int VOLUME_STEP     = 5;

    inline tsl::Color A(tsl::Color c) { return tsl::gfx::Renderer::a(c); }

    const char* repeatLabel(Repeat r) {
        switch (r) {
            case Repeat::Context: return "All";
            case Repeat::Track:   return "One";
            default:              return "Off";
        }
    }

    Repeat nextRepeat(Repeat r) {
        switch (r) {
            case Repeat::Off:     return Repeat::Context;
            case Repeat::Context: return Repeat::Track;
            default:              return Repeat::Off;
        }
    }

    // Longest prefix of `text` (on a UTF-8 boundary) that fits in maxWidth, with "..." when cut.
    std::string ellipsize(tsl::gfx::Renderer* r, const std::string& text, u32 fontSize, s32 maxWidth) {
        if (r->getTextDimensions(text, false, fontSize).first <= maxWidth) return text;

        std::vector<size_t> cuts;
        for (size_t i = 0; i < text.size(); i += utf8Len(static_cast<unsigned char>(text[i])))
            cuts.push_back(i);

        size_t lo = 0, hi = cuts.size();
        while (lo + 1 < hi) {
            const size_t mid = (lo + hi) / 2;
            if (r->getTextDimensions(text.substr(0, cuts[mid]) + "...", false, fontSize).first <= maxWidth)
                lo = mid;
            else
                hi = mid;
        }
        return text.substr(0, cuts[lo]) + "...";
    }

    // ellipsize() measures text several times; cache the result per line so it
    // only runs when the text changes, not every frame.
    class FittedText {
    public:
        const std::string& get(tsl::gfx::Renderer* r, const std::string& text, u32 fontSize, s32 maxWidth) {
            if (text != m_src || fontSize != m_fontSize || maxWidth != m_maxWidth) {
                m_src = text;
                m_fontSize = fontSize;
                m_maxWidth = maxWidth;
                m_out = ellipsize(r, text, fontSize, maxWidth);
            }
            return m_out;
        }

    private:
        std::string m_src, m_out;
        u32 m_fontSize = 0;
        s32 m_maxWidth = 0;
    };

    // Horizontally scrolling single line of text, used for long track titles.
    class Marquee {
    public:
        void draw(tsl::gfx::Renderer* r, const std::string& text, s32 x, s32 y, u32 fontSize, s32 maxWidth, tsl::Color color) {
            if (text != m_text || fontSize != m_fontSize) {
                m_text = text;
                m_fontSize = fontSize;
                m_width = r->getTextDimensions(text, false, fontSize).first;
                m_startMs = nowMs();
            }

            if (m_width <= maxWidth) {
                r->drawString(text, false, x, y, fontSize, A(color));
                return;
            }

            // pause at start, scroll to the end, pause, jump back
            static constexpr u64 PAUSE_MS = 1800;
            static constexpr float PX_PER_MS = 0.04f;
            const s32 overflow = m_width - maxWidth;
            const u64 scrollMs = static_cast<u64>(overflow / PX_PER_MS);
            const u64 t = (nowMs() - m_startMs) % (PAUSE_MS * 2 + scrollMs);

            s32 offset = 0;
            if (t > PAUSE_MS) offset = std::min<s32>(overflow, static_cast<s32>((t - PAUSE_MS) * PX_PER_MS));

            // Clip to our box, but never outside the list viewport (scissors don't nest).
            const s32 top = std::max<s32>(y - static_cast<s32>(fontSize), 97);
            const s32 bottom = std::min<s32>(y + 8, static_cast<s32>(tsl::cfg::FramebufferHeight) - 73);
            if (bottom <= top) return;
            r->enableScissoring(x, top, maxWidth, bottom - top);
            r->drawString(text, false, x - offset, y, fontSize, A(color));
            r->disableScissoring();
        }

    private:
        std::string m_text;
        u32 m_fontSize = 0;
        s32 m_width = 0;
        u64 m_startMs = 0;
    };

    void drawPlayIcon(tsl::gfx::Renderer* r, s32 x, s32 y, s32 size, tsl::Color c) {
        // Right-pointing triangle, one scanline at a time.
        for (s32 i = 0; i < size; ++i) {
            const s32 w = (i < size / 2 ? i : size - 1 - i) + 1;
            r->drawRect(x, y + i, w, 1, A(c));
        }
    }

    void drawPauseIcon(tsl::gfx::Renderer* r, s32 x, s32 y, s32 size, tsl::Color c) {
        const s32 bar = std::max<s32>(2, size / 3);
        r->drawRect(x, y, bar, size, A(c));
        r->drawRect(x + size - bar, y, bar, size, A(c));
    }

    bool messageVisible(const Snapshot& s) {
        return !s.message.empty() && nowMs() - s.messageAtMs < MESSAGE_SHOW_MS;
    }

}

// ----------------------------------------------------------------------------
// Devices
// ----------------------------------------------------------------------------

class GuiDevices : public tsl::Gui {
public:
    explicit GuiDevices(bool load = true) {
        if (load) Remote::get().post({Cmd::LoadDevices});
        Remote::get().devicesIfChanged(m_seen, m_data);
    }

    tsl::elm::Element* createUI() override {
        auto* frame = new tsl::elm::OverlayFrame("Devices", "Spotify Connect");
        auto* list = new tsl::elm::List();

        if (m_data.loading && m_data.items.empty()) {
            list->addItem(new tsl::elm::ListItem("Loading..."));
        } else if (!m_data.error.empty()) {
            list->addItem(new tsl::elm::ListItem("Error", m_data.error));
        } else if (m_data.items.empty()) {
            list->addItem(new tsl::elm::CategoryHeader("No devices found"));
            list->addItem(new tsl::elm::ListItem("Open Spotify on a phone/PC/speaker"));
        }

        if (!m_data.items.empty()) {
            list->addItem(new tsl::elm::CategoryHeader("Play on   Refresh"));
            for (const auto& d : m_data.items) {
                std::string value = d.active ? " Active" : d.type;
                auto* item = new tsl::elm::ListItem(d.name, value);
                const std::string id = d.id;
                const bool restricted = d.restricted;
                item->setClickListener([id, restricted](u64 keys) {
                    if (!(keys & KEY_A)) return false;
                    if (restricted) return true;
                    Remote::get().post({Cmd::Transfer, 0, id});
                    tsl::goBack();
                    return true;
                });
                list->addItem(item);
            }
        }

        frame->setContent(list);
        return frame;
    }

    void update() override {
        AsyncList<DeviceInfo> fresh;
        if (Remote::get().devicesIfChanged(m_seen, fresh) && !fresh.loading) {
            tsl::swapTo<GuiDevices>(false);  // rebuild with the loaded data
            return;
        }
    }

    bool handleInput(u64 keysDown, u64, const HidTouchState&, HidAnalogStickState, HidAnalogStickState) override {
        if (keysDown & KEY_Y) {
            tsl::swapTo<GuiDevices>(true);
            return true;
        }
        return false;
    }

private:
    AsyncList<DeviceInfo> m_data;
    u32 m_seen = 0;
};

// ----------------------------------------------------------------------------
// Playlist tracks
// ----------------------------------------------------------------------------

class GuiPlaylist : public tsl::Gui {
public:
    GuiPlaylist(PlaylistInfo playlist, bool load = true) : m_playlist(std::move(playlist)) {
        if (load) Remote::get().post({Cmd::LoadPlaylistTracks, 0, m_playlist.id});
        Remote::get().playlistTracksIfChanged(m_seen, m_data);
    }

    tsl::elm::Element* createUI() override {
        auto* frame = new tsl::elm::OverlayFrame(m_playlist.name, m_playlist.owner.empty() ? "Playlist" : "by " + m_playlist.owner);
        auto* list = new tsl::elm::List();

        const std::string uri = m_playlist.uri;
        auto* play = new tsl::elm::ListItem("Play");
        play->setClickListener([uri](u64 keys) {
            if (!(keys & KEY_A)) return false;
            Remote::get().post({Cmd::SetShuffle, 0});
            Remote::get().post({Cmd::PlayContext, 0, uri});
            return true;
        });
        list->addItem(play);

        auto* shuffle = new tsl::elm::ListItem("Shuffle play");
        shuffle->setClickListener([uri](u64 keys) {
            if (!(keys & KEY_A)) return false;
            Remote::get().post({Cmd::SetShuffle, 1});
            Remote::get().post({Cmd::PlayContext, 0, uri});
            return true;
        });
        list->addItem(shuffle);

        list->addItem(new tsl::elm::CategoryHeader(
            m_playlist.total ? "Tracks (" + std::to_string(m_playlist.total) + ")" : "Tracks"));

        if (m_data.loading && m_data.items.empty()) {
            list->addItem(new tsl::elm::ListItem("Loading..."));
        } else if (!m_data.error.empty() && m_data.items.empty()) {
            list->addItem(new tsl::elm::ListItem(m_data.error));
        }

        for (const auto& t : m_data.items) {
            auto* item = new tsl::elm::ListItem(t.artists.empty() ? t.name : t.name + " - " + t.artists,
                                                formatTime(t.durationMs));
            const std::string trackUri = t.uri;
            item->setClickListener([uri, trackUri](u64 keys) {
                if (!(keys & KEY_A)) return false;
                Remote::get().post({Cmd::PlayContext, 0, uri, trackUri});
                return true;
            });
            list->addItem(item);
        }

        frame->setContent(list);
        return frame;
    }

    void update() override {
        AsyncList<TrackInfo> fresh;
        if (Remote::get().playlistTracksIfChanged(m_seen, fresh) && !fresh.loading && fresh.key == m_playlist.id) {
            tsl::swapTo<GuiPlaylist>(m_playlist, false);
            return;
        }
    }

private:
    PlaylistInfo m_playlist;
    AsyncList<TrackInfo> m_data;
    u32 m_seen = 0;
};

// ----------------------------------------------------------------------------
// Playlists
// ----------------------------------------------------------------------------

class GuiPlaylists : public tsl::Gui {
public:
    explicit GuiPlaylists(bool load = true) {
        if (load) Remote::get().post({Cmd::LoadPlaylists});
        Remote::get().playlistsIfChanged(m_seen, m_data);
    }

    tsl::elm::Element* createUI() override {
        auto* frame = new tsl::elm::OverlayFrame("Playlists", "Your library");
        auto* list = new tsl::elm::List();

        if (m_data.loading && m_data.items.empty()) {
            list->addItem(new tsl::elm::ListItem("Loading..."));
        } else if (!m_data.error.empty() && m_data.items.empty()) {
            list->addItem(new tsl::elm::ListItem("Error", m_data.error));
        } else if (m_data.items.empty()) {
            list->addItem(new tsl::elm::ListItem("No playlists"));
        }

        if (!m_data.items.empty())
            list->addItem(new tsl::elm::CategoryHeader(" Open   Play now"));

        for (const auto& p : m_data.items) {
            auto* item = new tsl::elm::ListItem(p.name, p.total ? std::to_string(p.total) : "");
            const PlaylistInfo info = p;
            item->setClickListener([info](u64 keys) {
                if (keys & KEY_A) {
                    tsl::changeTo<GuiPlaylist>(info);
                    return true;
                }
                if (keys & KEY_Y) {
                    Remote::get().post({Cmd::PlayContext, 0, info.uri});
                    return true;
                }
                return false;
            });
            list->addItem(item);
        }

        frame->setContent(list);
        return frame;
    }

    void update() override {
        AsyncList<PlaylistInfo> fresh;
        if (Remote::get().playlistsIfChanged(m_seen, fresh) && !fresh.loading) {
            tsl::swapTo<GuiPlaylists>(false);
            return;
        }
    }

private:
    AsyncList<PlaylistInfo> m_data;
    u32 m_seen = 0;
};

// ----------------------------------------------------------------------------
// Queue
// ----------------------------------------------------------------------------

class GuiQueue : public tsl::Gui {
public:
    explicit GuiQueue(bool load = true) {
        if (load) Remote::get().post({Cmd::LoadQueue});
        Remote::get().queueIfChanged(m_seen, m_data);
    }

    tsl::elm::Element* createUI() override {
        auto* frame = new tsl::elm::OverlayFrame("Queue", "Up next");
        auto* list = new tsl::elm::List();

        if (m_data.loading && m_data.items.empty()) {
            list->addItem(new tsl::elm::ListItem("Loading..."));
        } else if (!m_data.error.empty()) {
            list->addItem(new tsl::elm::ListItem("Error", m_data.error));
        }

        for (size_t i = 0; i < m_data.items.size(); ++i) {
            const TrackInfo& t = m_data.items[i];
            if (i == 0) {
                if (t.uri.empty()) continue;
                list->addItem(new tsl::elm::CategoryHeader("Now playing"));
            } else if (i == 1) {
                list->addItem(new tsl::elm::CategoryHeader("Next up   Skip to track"));
            }

            auto* item = new tsl::elm::ListItem(t.artists.empty() ? t.name : t.name + " - " + t.artists,
                                                formatTime(t.durationMs));
            const int skips = static_cast<int>(i);
            item->setClickListener([skips](u64 keys) {
                if (!(keys & KEY_A) || skips == 0) return false;
                Remote::get().post({Cmd::SkipForward, skips});
                tsl::swapTo<GuiQueue>(true);
                return true;
            });
            list->addItem(item);
        }

        if (!m_data.loading && m_data.error.empty() && m_data.items.size() <= 1)
            list->addItem(new tsl::elm::ListItem("Queue is empty"));

        frame->setContent(list);
        return frame;
    }

    void update() override {
        AsyncList<TrackInfo> fresh;
        if (Remote::get().queueIfChanged(m_seen, fresh) && !fresh.loading) {
            tsl::swapTo<GuiQueue>(false);
            return;
        }
    }

private:
    AsyncList<TrackInfo> m_data;
    u32 m_seen = 0;
};

// ----------------------------------------------------------------------------
// Setup
// ----------------------------------------------------------------------------

class GuiSetup : public tsl::Gui {
public:
    GuiSetup() {
        SetupServer::get().start();
        m_url = SetupServer::get().url();
    }

    ~GuiSetup() override {
        SetupServer::get().stop();
    }

    tsl::elm::Element* createUI() override {
        auto* frame = new tsl::elm::OverlayFrame("Setup", "Link your Spotify account");
        auto* list = new tsl::elm::List();

        list->addItem(new tsl::elm::CategoryHeader("From a phone or PC on the same Wi-Fi"));
        list->addItem(new tsl::elm::CustomDrawer([this](tsl::gfx::Renderer* r, s32 x, s32 y, s32 w, s32) {
            const s32 tx = x + 12;
            r->drawString("Open this address in a browser:", false, tx, y + 26, 18, A(COLOR_SUB));
            if (m_url.empty()) {
                r->drawString("No network - connect to Wi-Fi", false, tx, y + 62, 22, A(COLOR_ERROR));
            } else {
                r->drawString(m_url, false, tx, y + 62, 21, A(COLOR_GREEN));
            }
            r->drawString(m_statusText.get(r, m_status, 16, w - 24), false, tx, y + 96, 16, A(m_linked ? COLOR_GREEN : COLOR_SUB));
            r->drawString("Keep this screen open until the page says", false, tx, y + 126, 15, A(COLOR_DIM));
            r->drawString("\"All set!\".", false, tx, y + 146, 15, A(COLOR_DIM));
            if (!m_url.empty())
                r->drawString("Debug log: " + m_url + "log", false, tx, y + 172, 15, A(COLOR_DIM));
        }), 190);

        list->addItem(new tsl::elm::CategoryHeader("Or from a PC"));
        list->addItem(new tsl::elm::CustomDrawer([](tsl::gfx::Renderer* r, s32 x, s32 y, s32, s32) {
            const s32 tx = x + 12;
            r->drawString("Run tools/spotify_auth.py and copy the", false, tx, y + 24, 16, A(COLOR_SUB));
            r->drawString("generated config.ini to the SD card:", false, tx, y + 46, 16, A(COLOR_SUB));
            r->drawString("/switch/spotify-remote/config.ini", false, tx, y + 72, 17, A(COLOR_TEXT));
        }), 90);

        auto* reload = new tsl::elm::ListItem("Reload config.ini");
        reload->setClickListener([](u64 keys) {
            if (!(keys & KEY_A)) return false;
            Remote::get().post({Cmd::ReloadConfig});
            return true;
        });
        list->addItem(reload);

        frame->setContent(list);
        return frame;
    }

    void update() override {
        // Polling the status string a few times per second is plenty.
        if (nowMs() - m_lastPollMs < 250) return;
        m_lastPollMs = nowMs();
        m_status = SetupServer::get().status();
        m_linked = m_status.rfind("Linked", 0) == 0;
        if (m_url.empty()) m_url = SetupServer::get().url();
    }

private:
    std::string m_url;
    std::string m_status;
    FittedText m_statusText;
    bool m_linked = false;
    u64 m_lastPollMs = 0;
};

// ----------------------------------------------------------------------------
// Main / now playing
// ----------------------------------------------------------------------------

class GuiMain : public tsl::Gui {
public:
    GuiMain() {
        Remote::get().snapshotIfChanged(m_seen, m_snap);
    }

    tsl::elm::Element* createUI() override {
        auto* frame = new tsl::elm::OverlayFrame("Spotify Remote", APP_VERSION);
        auto* list = new tsl::elm::List();

        list->addItem(new tsl::elm::CustomDrawer([this](tsl::gfx::Renderer* r, s32 x, s32 y, s32 w, s32 h) {
            drawCard(r, x, y, w, h);
        }), CARD_HEIGHT);

        list->addItem(new tsl::elm::CategoryHeader("Playback"));

        m_playItem = new tsl::elm::ListItem("Play / Pause");
        m_playItem->setClickListener([this](u64 keys) {
            if (keys & KEY_A)     { togglePlay(); return true; }
            if (keys & KEY_LEFT)  { Remote::get().post({Cmd::Previous}); return true; }
            if (keys & KEY_RIGHT) { Remote::get().post({Cmd::Next}); return true; }
            return false;
        });
        list->addItem(m_playItem);

        auto* next = new tsl::elm::ListItem("Next track");
        next->setClickListener([](u64 keys) {
            if (!(keys & KEY_A)) return false;
            Remote::get().post({Cmd::Next});
            return true;
        });
        list->addItem(next);

        auto* prev = new tsl::elm::ListItem("Previous track");
        prev->setClickListener([](u64 keys) {
            if (!(keys & KEY_A)) return false;
            Remote::get().post({Cmd::Previous});
            return true;
        });
        list->addItem(prev);

        m_seekItem = new tsl::elm::ListItem("Seek");
        m_seekItem->setClickListener([this](u64 keys) {
            if (keys & KEY_LEFT)  { seekBy(-static_cast<s32>(SEEK_STEP_MS)); return true; }
            if (keys & (KEY_RIGHT | KEY_A)) { seekBy(SEEK_STEP_MS); return true; }
            return false;
        });
        list->addItem(m_seekItem);

        m_shuffleItem = new tsl::elm::ToggleListItem("Shuffle", m_snap.player.shuffle);
        m_shuffleItem->setStateChangedListener([](bool state) {
            Remote::get().post({Cmd::SetShuffle, state ? 1 : 0});
        });
        list->addItem(m_shuffleItem);

        m_repeatItem = new tsl::elm::ListItem("Repeat", repeatLabel(m_snap.player.repeat));
        m_repeatItem->setClickListener([this](u64 keys) {
            if (!(keys & KEY_A)) return false;
            Remote::get().post({Cmd::SetRepeat, static_cast<int>(nextRepeat(m_snap.player.repeat))});
            return true;
        });
        list->addItem(m_repeatItem);

        m_likeItem = new tsl::elm::ListItem("Save to Liked Songs");
        m_likeItem->setClickListener([this](u64 keys) {
            if (!(keys & KEY_A)) return false;
            if (m_snap.player.track.uri.empty()) return true;
            Remote::get().post({Cmd::SetLike, m_snap.liked ? 0 : 1, m_snap.player.track.uri});
            return true;
        });
        list->addItem(m_likeItem);

        list->addItem(new tsl::elm::CategoryHeader("Volume"));
        m_volumeBar = new tsl::elm::TrackBar("", false, false, true, "Volume", "%");
        m_volumeBar->setProgress(m_snap.player.device.volume >= 0 ? m_snap.player.device.volume : 0);
        m_volumeBar->setValueChangedListener([this](u16 value) {
            m_volumeTouchedMs = nowMs();
            Remote::get().setVolume(value);
        });
        list->addItem(m_volumeBar);

        list->addItem(new tsl::elm::CategoryHeader("Browse"));

        m_deviceItem = new tsl::elm::ListItem("Devices");
        m_deviceItem->setClickListener([](u64 keys) {
            if (!(keys & KEY_A)) return false;
            tsl::changeTo<GuiDevices>(true);
            return true;
        });
        list->addItem(m_deviceItem);

        auto* playlists = new tsl::elm::ListItem("Playlists");
        playlists->setClickListener([](u64 keys) {
            if (!(keys & KEY_A)) return false;
            tsl::changeTo<GuiPlaylists>(true);
            return true;
        });
        list->addItem(playlists);

        auto* queue = new tsl::elm::ListItem("Queue");
        queue->setClickListener([](u64 keys) {
            if (!(keys & KEY_A)) return false;
            tsl::changeTo<GuiQueue>(true);
            return true;
        });
        list->addItem(queue);

        list->addItem(new tsl::elm::CategoryHeader("Settings"));

        auto* setup = new tsl::elm::ListItem("Setup / link account");
        setup->setClickListener([](u64 keys) {
            if (!(keys & KEY_A)) return false;
            tsl::changeTo<GuiSetup>();
            return true;
        });
        list->addItem(setup);

        auto* reload = new tsl::elm::ListItem("Reload config.ini");
        reload->setClickListener([](u64 keys) {
            if (!(keys & KEY_A)) return false;
            Remote::get().post({Cmd::ReloadConfig});
            return true;
        });
        list->addItem(reload);

        refreshItems();
        frame->setContent(list);
        return frame;
    }

    void update() override {
        if (Remote::get().snapshotIfChanged(m_seen, m_snap))
            refreshItems();

        // The seek value shows the live position, so refresh it once a second.
        const u32 sec = m_snap.currentProgressMs() / 1000;
        if (sec != m_lastSeekSec) {
            m_lastSeekSec = sec;
            updateSeekValue();
        }
    }

    bool handleInput(u64 keysDown, u64, const HidTouchState&, HidAnalogStickState, HidAnalogStickState rightStick) override {
        if (keysDown & KEY_Y) {
            togglePlay();
            return true;
        }

        // Right stick: left/right = previous/next, up/down = volume.
        static constexpr s32 ON = 20000, OFF = 8000;
        const s32 sx = rightStick.x, sy = rightStick.y;
        int dir = 0;
        if (sx > ON) dir = 1; else if (sx < -ON) dir = 2; else if (sy > ON) dir = 3; else if (sy < -ON) dir = 4;

        if (dir == 0) {
            if (std::abs(sx) < OFF && std::abs(sy) < OFF) m_stickDir = 0;
            return false;
        }

        const u64 now = nowMs();
        const bool isVolume = dir >= 3;
        // Tracks fire once per flick; volume repeats while held.
        if (dir == m_stickDir && (!isVolume || now - m_stickRepeatMs < 150)) return false;
        m_stickDir = dir;
        m_stickRepeatMs = now;

        switch (dir) {
            case 1: Remote::get().post({Cmd::Next}); break;
            case 2: Remote::get().post({Cmd::Previous}); break;
            default: {
                const int cur = m_snap.player.device.volume >= 0 ? m_snap.player.device.volume : m_volumeBar->getProgress();
                const int vol = std::clamp(cur + (dir == 3 ? VOLUME_STEP : -VOLUME_STEP), 0, 100);
                m_volumeTouchedMs = now;
                m_volumeBar->setProgress(vol);
                Remote::get().setVolume(vol);
                break;
            }
        }
        return true;
    }

private:
    static constexpr u16 CARD_HEIGHT = 196;

    void togglePlay() {
        Remote::get().post({Cmd::SetPlaying, m_snap.player.playing ? 0 : 1});
    }

    void seekBy(s32 deltaMs) {
        if (!m_snap.player.hasPlayback) return;
        const s64 pos = static_cast<s64>(m_snap.currentProgressMs()) + deltaMs;
        Remote::get().seekTo(static_cast<u32>(std::max<s64>(0, pos)));
    }

    void updateSeekValue() {
        if (!m_seekItem) return;
        if (!m_snap.player.hasPlayback) {
            m_seekItem->setValue("");
            return;
        }
        m_seekItem->setValue(" " + formatTime(m_snap.currentProgressMs()) + " / " +
                             formatTime(m_snap.player.track.durationMs) + " ");
    }

    void refreshItems() {
        const PlayerState& p = m_snap.player;

        m_playItem->setValue(!p.hasPlayback ? "" : p.playing ? "Playing" : "Paused");

        if (m_shuffleItem->getState() != p.shuffle)
            m_shuffleItem->setState(p.shuffle);

        m_repeatItem->setValue(repeatLabel(p.repeat));

        if (!m_snap.likedKnown || p.track.uri.empty())
            m_likeItem->setValue("");
        else
            m_likeItem->setValue(m_snap.liked ? "★ Saved" : "Not saved", !m_snap.liked);

        // Don't fight the user while they're dragging the slider.
        if (p.device.volume >= 0 && nowMs() - m_volumeTouchedMs > 3000)
            m_volumeBar->setProgress(static_cast<u16>(p.device.volume));

        m_deviceItem->setValue(p.device.name);
        updateSeekValue();
    }

    void drawCard(tsl::gfx::Renderer* r, s32 x, s32 y, s32 w, s32) {
        const Snapshot& s = m_snap;
        const s32 left = x + 12;
        const s32 textW = w - 24;

        if (!s.configured || !s.authorized) {
            r->drawString(s.configured ? "Almost there" : "Not linked to Spotify yet", false, left, y + 44, 24, A(COLOR_TEXT));
            r->drawString("Open \"Setup / link account\" below and", false, left, y + 80, 17, A(COLOR_SUB));
            r->drawString("follow the steps on your phone or PC,", false, left, y + 104, 17, A(COLOR_SUB));
            r->drawString("or put config.ini in", false, left, y + 128, 17, A(COLOR_SUB));
            r->drawString("/switch/spotify-remote/", false, left, y + 152, 17, A(COLOR_TEXT));
            return;
        }

        // Status line: play state + device, or a recent message.
        const s32 statusY = y + 26;
        if (messageVisible(s)) {
            r->drawString(m_statusLine.get(r, s.message, 16, textW), false, left, statusY, 16,
                          A(s.messageIsError ? COLOR_ERROR : COLOR_GREEN));
        } else if (s.player.hasPlayback) {
            if (s.player.playing)
                drawPlayIcon(r, left, statusY - 13, 14, COLOR_GREEN);
            else
                drawPauseIcon(r, left, statusY - 13, 13, COLOR_SUB);
            const std::string where = (s.player.playing ? "Playing on " : "Paused on ") + s.player.device.name;
            r->drawString(m_statusLine.get(r, where, 16, textW - 24), false, left + 22, statusY, 16,
                          A(s.player.playing ? COLOR_GREEN : COLOR_SUB));
        }

        if (!s.loaded) {
            r->drawString("Connecting to Spotify...", false, left, y + 80, 22, A(COLOR_SUB));
            return;
        }

        if (!s.player.hasPlayback) {
            if (!s.connected) {
                r->drawString("Can't reach Spotify", false, left, y + 70, 24, A(COLOR_TEXT));
                r->drawString(m_errorLine.get(r, s.message, 16, textW), false, left, y + 100, 16, A(COLOR_ERROR));
            } else {
                r->drawString("Nothing playing", false, left, y + 70, 24, A(COLOR_TEXT));
                r->drawString("Press  to resume on your last device", false, left, y + 102, 17, A(COLOR_SUB));
                r->drawString("or pick one in Devices.", false, left, y + 126, 17, A(COLOR_SUB));
            }
            return;
        }

        const TrackInfo& t = s.player.track;
        m_titleMarquee.draw(r, t.name.empty() ? "Unknown" : t.name, left, y + 62, 25, textW, COLOR_TEXT);
        m_artistMarquee.draw(r, t.artists, left, y + 90, 19, textW, COLOR_SUB);
        if (!t.album.empty())
            r->drawString(m_albumLine.get(r, t.album, 16, textW), false, left, y + 114, 16, A(COLOR_DIM));

        // Progress bar
        const u32 pos = s.currentProgressMs();
        const s32 barY = y + 134, barH = 6;
        r->drawRect(left, barY, textW, barH, A(COLOR_TRACK));
        if (t.durationMs) {
            const s32 fill = static_cast<s32>(static_cast<u64>(textW) * std::min(pos, t.durationMs) / t.durationMs);
            r->drawRect(left, barY, fill, barH, A(COLOR_GREEN));
            r->drawCircle(left + fill, barY + barH / 2, 6, true, A(COLOR_TEXT));
        }

        const std::string cur = formatTime(pos);
        const std::string dur = formatTime(t.durationMs);
        r->drawString(cur, false, left, y + 162, 15, A(COLOR_SUB));
        const s32 durW = r->getTextDimensions(dur, false, 15).first;
        r->drawString(dur, false, left + textW - durW, y + 162, 15, A(COLOR_SUB));

        std::string flags;
        if (s.player.shuffle) flags += "Shuffle";
        if (s.player.repeat != Repeat::Off) flags += std::string(flags.empty() ? "" : "  ") + "Repeat " + repeatLabel(s.player.repeat);
        if (!flags.empty()) {
            const s32 fw = r->getTextDimensions(flags, false, 15).first;
            r->drawString(flags, false, left + (textW - fw) / 2, y + 162, 15, A(COLOR_GREEN));
        }

        r->drawString(" Play/Pause    ←→ Skip    ↑↓ Volume", false, left, y + 188, 14, A(COLOR_DIM));
    }

    Snapshot m_snap;
    u32 m_seen = 0;
    u32 m_lastSeekSec = ~0u;
    u64 m_volumeTouchedMs = 0;
    int m_stickDir = 0;
    u64 m_stickRepeatMs = 0;

    Marquee m_titleMarquee;
    Marquee m_artistMarquee;
    FittedText m_statusLine, m_errorLine, m_albumLine;

    tsl::elm::ListItem* m_playItem = nullptr;
    tsl::elm::ListItem* m_seekItem = nullptr;
    tsl::elm::ToggleListItem* m_shuffleItem = nullptr;
    tsl::elm::ListItem* m_repeatItem = nullptr;
    tsl::elm::ListItem* m_likeItem = nullptr;
    tsl::elm::TrackBar* m_volumeBar = nullptr;
    tsl::elm::ListItem* m_deviceItem = nullptr;
};

// ----------------------------------------------------------------------------
// Overlay
// ----------------------------------------------------------------------------

class SpotifyOverlay : public tsl::Overlay {
public:
    void initServices() override {
        nifmInitialize(NifmServiceType_User);

        // Small buffers: the overlay heap is only a few MB and Spotify
        // responses are small.
        static constexpr SocketInitConfig socketConfig = {
            .tcp_tx_buf_size     = 16 * 1024,
            .tcp_rx_buf_size     = 32 * 1024,
            .tcp_tx_buf_max_size = 32 * 1024,
            .tcp_rx_buf_max_size = 64 * 1024,
            .udp_tx_buf_size     = 512,
            .udp_rx_buf_size     = 512,
            .sb_efficiency       = 1,
            .bsd_service_type    = BsdServiceType_Auto,
        };
        m_socketOk = R_SUCCEEDED(socketInitialize(&socketConfig));
        curl_global_init(CURL_GLOBAL_DEFAULT);
        Http::abortAll(false);

        Remote::get().start();
    }

    void exitServices() override {
        Http::abortAll(true);
        SetupServer::get().stop();
        Remote::get().stop();
        curl_global_cleanup();
        if (m_socketOk) socketExit();
        nifmExit();
    }

    void onShow() override { Remote::get().setVisible(true); }
    void onHide() override { Remote::get().setVisible(false); }

    std::unique_ptr<tsl::Gui> loadInitialGui() override {
        return initially<GuiMain>();
    }

private:
    bool m_socketOk = false;
};

int main(int argc, char** argv) {
    return tsl::loop<SpotifyOverlay>(argc, argv);
}
