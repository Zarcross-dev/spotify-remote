// Host-side tests: runs the overlay's worker, API client, config and setup
// server against mock_spotify.py. Build/run with tests/host/run.sh.

#include "config.hpp"
#include "http.hpp"
#include "log.hpp"
#include "remote.hpp"
#include "setup_server.hpp"
#include "util.hpp"

#include <cstdio>
#include <functional>
#include <string>
#include <thread>
#include <sys/stat.h>
#include <unistd.h>

using namespace spr;

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond) do { ++g_checks; if (!(cond)) { ++g_failures; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

static const std::string MOCK = "http://127.0.0.1:18080";

static bool waitFor(const std::function<bool()>& pred, int timeoutMs = 4000) {
    for (int t = 0; t < timeoutMs; t += 25) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    return pred();
}

static Http& ctl() { static Http h; return h; }

static std::string mockState() { return ctl().request("GET", MOCK + "/_state").body; }
static void mockSet(const std::string& json) { ctl().request("POST", MOCK + "/_set", {"Content-Type: application/json"}, json); }
static bool stateHas(const std::string& needle) { return mockState().find(needle) != std::string::npos; }
static std::string mockLog() { return ctl().request("GET", MOCK + "/_log").body; }
static size_t countInLog(const std::string& needle) {
    const std::string log = mockLog();
    size_t n = 0;
    for (size_t p = log.find(needle); p != std::string::npos; p = log.find(needle, p + 1)) ++n;
    return n;
}

static void testUtil() {
    CHECK(base64Encode("cid:csecret") == "Y2lkOmNzZWNyZXQ=");
    CHECK(base64Encode("a") == "YQ==");
    CHECK(base64Encode("ab") == "YWI=");
    CHECK(base64Encode("abc") == "YWJj");
    CHECK(urlEncode("spotify:track:x y") == "spotify%3Atrack%3Ax%20y");
    CHECK(urlDecode("a%3Ab+c") == "a:b c");
    CHECK(queryParam("code=AbC%2F1&state=xyz", "code") == "AbC/1");
    CHECK(queryParam("code=AbC&state=xyz", "state") == "xyz");
    CHECK(queryParam("code=AbC", "missing").empty());
    CHECK(formatTime(0) == "0:00");
    CHECK(formatTime(61000) == "1:01");
    CHECK(formatTime(3723000) == "1:02:03");
    CHECK(htmlEscape("<a href='x'>") == "&lt;a href=&#39;x&#39;&gt;");
    CHECK(jsonEscape("a\"b\\c\n") == "a\\\"b\\\\c\\n");
}

static void testConfig() {
    Config c;
    c.clientId = "id1"; c.clientSecret = "sec"; c.refreshToken = "rt"; c.pollMs = 1500; c.sslVerify = false;
    CHECK(c.save());
    Config d;
    CHECK(d.load());
    CHECK(d.clientId == "id1" && d.clientSecret == "sec" && d.refreshToken == "rt");
    CHECK(d.pollMs == 1500 && !d.sslVerify);
    CHECK(Config::update([](Config& x) { x.deviceId = "devX"; }));
    CHECK(d.load() && d.deviceId == "devX" && d.clientId == "id1");

    // Hand-written file with spaces, quotes and comments.
    FILE* f = fopen(CONFIG_PATH, "w");
    fputs("# comment\n[spotify]\n client_id = \"0123456789abcdef0123456789abcdef\" \nclient_secret='fedcba9876543210fedcba9876543210'\r\nrefresh_token=RT1\npoll_interval_ms=10\n", f);
    fclose(f);
    CHECK(d.load());
    CHECK(d.clientId == "0123456789abcdef0123456789abcdef" && d.clientSecret == "fedcba9876543210fedcba9876543210" && d.refreshToken == "RT1");
    CHECK(d.pollMs == 2000);  // out of range -> default
    CHECK(d.sslVerify);
}

static void testRemote() {
    Remote& r = Remote::get();
    r.start();

    Snapshot s;
    u32 seen = 0;
    auto snap = [&] { r.snapshotIfChanged(seen, s); return s; };

    // Initial poll: needs a token refresh, survives the forced 401, picks up rotation.
    CHECK(waitFor([&] { return snap().loaded && s.player.hasPlayback; }));
    CHECK(s.configured && s.authorized && s.connected);
    CHECK(s.player.track.name == "Song 0");
    CHECK(s.player.track.artists == "Artist 0, Feat");
    CHECK(s.player.track.album == "Album 0");
    CHECK(s.player.device.name == "Kitchen");
    CHECK(s.player.device.volume == 40);
    CHECK(s.player.playing);
    CHECK(s.likedKnown && !s.liked);
    CHECK(waitFor([] { Config c; c.load(); return c.refreshToken == "RT2"; }));
    { Config c; c.load(); CHECK(c.deviceId == "dev1"); }

    // Pause: optimistic UI change is immediate, server follows.
    r.post({Cmd::SetPlaying, 0});
    CHECK(!r.snapshot().player.playing);
    CHECK(waitFor([] { return stateHas("\"playing\": false"); }));

    r.post({Cmd::SetPlaying, 1});
    CHECK(waitFor([] { return stateHas("\"playing\": true"); }));

    r.post({Cmd::Next});
    CHECK(waitFor([&] { return snap().player.track.name == "Song 1"; }));
    r.post({Cmd::Previous});
    CHECK(waitFor([&] { return snap().player.track.name == "Song 0"; }));

    r.post({Cmd::SetShuffle, 1});
    CHECK(r.snapshot().player.shuffle);
    CHECK(waitFor([] { return stateHas("\"shuffle\": true"); }));

    r.post({Cmd::SetRepeat, static_cast<int>(Repeat::Track)});
    CHECK(waitFor([] { return stateHas("\"repeat\": \"track\""); }));
    r.post({Cmd::SetRepeat, static_cast<int>(Repeat::Context)});
    CHECK(waitFor([] { return stateHas("\"repeat\": \"context\""); }));

    // Rapid volume changes are coalesced; the final value wins.
    const size_t volBefore = countInLog("/v1/me/player/volume");
    for (int v = 41; v <= 60; ++v) {
        r.setVolume(v);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(waitFor([] { return stateHas("\"volume\": 60"); }));
    const size_t volSent = countInLog("/v1/me/player/volume") - volBefore;
    CHECK(volSent >= 1 && volSent <= 4);
    fprintf(stderr, "  volume: 20 changes -> %zu requests\n", volSent);

    r.seekTo(123456);
    CHECK(waitFor([] { return stateHas("\"progress\": 123456"); }));

    // Like / unlike via the Feb 2026 /me/library endpoint.
    r.post({Cmd::SetLike, 1, "spotify:track:t0"});
    CHECK(waitFor([] { return stateHas("spotify:track:t0"); }));
    r.post({Cmd::SetLike, 0, "spotify:track:t0"});
    CHECK(waitFor([] { return stateHas("\"saved\": []"); }));

    // Lists
    AsyncList<DeviceInfo> devs; u32 dv = 0;
    r.post({Cmd::LoadDevices});
    CHECK(waitFor([&] { return r.devicesIfChanged(dv, devs) && !devs.loading; }));
    CHECK(devs.items.size() == 2 && devs.items[1].name == "PC" && devs.items[0].active);

    AsyncList<PlaylistInfo> pls; u32 pv = 0;
    r.post({Cmd::LoadPlaylists});
    CHECK(waitFor([&] { r.playlistsIfChanged(pv, pls); return !pls.loading && !pls.items.empty(); }));
    CHECK(pls.items.size() == 30);
    CHECK(pls.items[7].name == "Playlist 7" && pls.items[7].total == 7 && pls.items[7].owner == "me");

    AsyncList<TrackInfo> tracks; u32 tv = 0;
    r.post({Cmd::LoadPlaylistTracks, 0, "pl1"});
    CHECK(waitFor([&] { r.playlistTracksIfChanged(tv, tracks); return !tracks.loading && !tracks.items.empty(); }));
    CHECK(tracks.items.size() == 4);              // both "item" and legacy "track" entries parsed
    CHECK(tracks.key == "pl1");

    r.post({Cmd::LoadPlaylistTracks, 0, "forbidden"});
    CHECK(waitFor([&] { r.playlistTracksIfChanged(tv, tracks); return !tracks.loading && tracks.key == "forbidden"; }));
    CHECK(tracks.items.empty() && tracks.error.find("own") != std::string::npos);

    AsyncList<TrackInfo> queue; u32 qv = 0;
    r.post({Cmd::LoadQueue});
    CHECK(waitFor([&] { r.queueIfChanged(qv, queue); return !queue.loading && !queue.items.empty(); }));
    CHECK(queue.items.size() == 4 && queue.items[0].name == "Song 0" && queue.items[1].name == "Song 1");

    r.post({Cmd::SkipForward, 2});
    CHECK(waitFor([&] { return snap().player.track.name == "Song 2"; }));

    r.post({Cmd::PlayContext, 0, "spotify:playlist:pl1", "spotify:track:t3"});
    CHECK(waitFor([&] { return snap().player.track.name == "Song 3"; }));

    r.post({Cmd::Transfer, 0, "dev2"});
    CHECK(waitFor([&] { return snap().player.device.name == "PC"; }));
    { Config c; c.load(); CHECK(c.deviceId == "dev2"); }

    // Nothing active: play falls back to the last used device.
    mockSet("{\"active\": false, \"playing\": false, \"device\": \"dev1\"}");
    r.post({Cmd::Refresh});
    CHECK(waitFor([&] { return snap().loaded && !s.player.hasPlayback; }));
    r.post({Cmd::SetPlaying, 1});
    CHECK(waitFor([] { return stateHas("\"active\": true") && stateHas("\"playing\": true") && stateHas("\"device\": \"dev2\""); }));

    // Errors surface as messages and don't kill the worker.
    mockSet("{\"active\": false}");
    r.post({Cmd::Next});
    CHECK(waitFor([&] { return snap().message.find("No active device") != std::string::npos && s.messageIsError; }));
    mockSet("{\"active\": true}");

    // Hidden overlay = no polling.
    r.setVisible(false);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    const size_t pollsBefore = countInLog("\"path\": \"/v1/me/player\", \"query\": {\"additional_types\"");
    std::this_thread::sleep_for(std::chrono::milliseconds(2500));
    CHECK(countInLog("\"path\": \"/v1/me/player\", \"query\": {\"additional_types\"") == pollsBefore);
    r.setVisible(true);
    CHECK(waitFor([&] { return countInLog("\"path\": \"/v1/me/player\", \"query\": {\"additional_types\"") > pollsBefore; }));

    // Revoked login -> clear error instead of a crash loop.
    Config::update([](Config& c) { c.refreshToken = "revoked"; });
    r.post({Cmd::ReloadConfig});
    CHECK(waitFor([&] { return snap().message.find("Login expired") != std::string::npos; }));
}

static std::string between(const std::string& s, const std::string& a, const std::string& b) {
    const size_t i = s.find(a);
    if (i == std::string::npos) return {};
    const size_t j = s.find(b, i + a.size());
    return s.substr(i + a.size(), j - i - a.size());
}

static void testSetupServer() {
    SetupServer& srv = SetupServer::get();
    CHECK(srv.start());
    const std::string url = srv.url();
    CHECK(url.rfind("http://127.0.0.1:8888/", 0) == 0 && url.size() == 29 && url.back() == '/');

    Http h;
    const std::string base = url.substr(0, url.size() - 1);  // http://127.0.0.1:8888/<key>

    // Without the key nothing is reachable.
    HttpResponse r0 = h.request("GET", "http://127.0.0.1:8888/");
    CHECK(r0.status == 403);
    r0 = h.request("GET", "http://127.0.0.1:8888/log.txt");
    CHECK(r0.status == 403);
    r0 = h.request("POST", "http://127.0.0.1:8888/save", {}, "client_id=x");
    CHECK(r0.status == 403);
    r0 = h.request("GET", base);
    CHECK(r0.status == 301);

    HttpResponse r = h.request("GET", base + "/");
    CHECK(r.status == 200 && r.body.find("Client ID") != std::string::npos);
    CHECK(r.body.find("http://127.0.0.1:8888/callback") != std::string::npos);

    // Malformed credentials (e.g. newline injection into config.ini) are refused.
    r = h.request("POST", base + "/save", {"Content-Type: application/x-www-form-urlencoded"},
                  "client_id=" + urlEncode("0123456789abcdef0123456789abcdef\nrefresh_token=evil") + "&client_secret=fedcba9876543210fedcba9876543210");
    CHECK(r.body.find("must be copied exactly") != std::string::npos);
    { Config c; c.load(); CHECK(c.refreshToken != "evil"); }

    // Secret left empty keeps the saved one; changed client id clears the old token.
    Config::update([](Config& c) { c.clientId = "old"; c.clientSecret = "fedcba9876543210fedcba9876543210"; c.refreshToken = "RT2"; });
    r = h.request("POST", base + "/save", {"Content-Type: application/x-www-form-urlencoded"}, "client_id=0123456789abcdef0123456789abcdef&client_secret=");
    CHECK(r.status == 200 && r.body.find("Log in with Spotify") != std::string::npos);
    { Config c; c.load(); CHECK(c.clientId == "0123456789abcdef0123456789abcdef" && c.clientSecret == "fedcba9876543210fedcba9876543210" && c.refreshToken.empty()); }

    const std::string authUrl = between(r.body, "class=btn href='", "'");
    CHECK(authUrl.find("client_id=0123456789abcdef0123456789abcdef") != std::string::npos);
    CHECK(authUrl.find("redirect_uri=http%3A%2F%2F127.0.0.1%3A8888%2Fcallback") != std::string::npos);
    CHECK(authUrl.find("user-modify-playback-state") != std::string::npos);
    const std::string state = between(authUrl + "&", "state=", "&");
    CHECK(state.size() == 32);

    // Wrong state is rejected.
    r = h.request("POST", base + "/code", {"Content-Type: application/x-www-form-urlencoded"},
                  "url=" + urlEncode("http://127.0.0.1:8888/callback?code=goodcode&state=nope"));
    CHECK(r.body.find("valid login code") != std::string::npos);

    // Missing state is rejected too.
    r = h.request("POST", base + "/code", {"Content-Type: application/x-www-form-urlencoded"},
                  "url=" + urlEncode("http://127.0.0.1:8888/callback?code=goodcode"));
    CHECK(r.body.find("valid login code") != std::string::npos);

    // Bad code -> Spotify error shown.
    r = h.request("POST", base + "/code", {"Content-Type: application/x-www-form-urlencoded"},
                  "url=" + urlEncode("http://127.0.0.1:8888/callback?code=badcode&state=" + state));
    CHECK(r.body.find("Login failed") != std::string::npos);

    // A state is single use: a fresh login link is needed after a failure.
    r = h.request("POST", base + "/code", {"Content-Type: application/x-www-form-urlencoded"},
                  "url=" + urlEncode("http://127.0.0.1:8888/callback?code=goodcode&state=" + state));
    CHECK(r.body.find("valid login code") != std::string::npos);
    r = h.request("POST", base + "/save", {"Content-Type: application/x-www-form-urlencoded"}, "client_id=0123456789abcdef0123456789abcdef&client_secret=");
    const std::string state2 = between(between(r.body, "class=btn href='", "'") + "&", "state=", "&");
    CHECK(state2.size() == 32 && state2 != state);

    // Pasted redirect URL -> refresh token saved, worker reloads and plays on.
    r = h.request("POST", base + "/code", {"Content-Type: application/x-www-form-urlencoded"},
                  "url=" + urlEncode("http://127.0.0.1:8888/callback?code=goodcode&state=" + state2));
    CHECK(r.body.find("All set") != std::string::npos);
    { Config c; c.load(); CHECK(c.refreshToken == "RT_NEW"); }
    CHECK(srv.status().rfind("Linked", 0) == 0);

    Snapshot s; u32 seen = 0;
    CHECK(waitFor([&] { Remote::get().snapshotIfChanged(seen, s); return s.authorized && s.loaded && s.connected; }));

    r = h.request("GET", base + "/nope");
    CHECK(r.status == 404);

    // Debug log: page, plain text, no secrets, clear.
    r = h.request("GET", base + "/log");
    CHECK(r.status == 200 && r.body.find("fetch('log.txt'") != std::string::npos);
    r = h.request("GET", base + "/log.txt");
    CHECK(r.status == 200);
    CHECK(r.body.find("credentials saved") != std::string::npos);
    CHECK(r.body.find("code exchange OK") != std::string::npos);
    CHECK(r.body.find("GET /me/player") != std::string::npos);
    CHECK(r.body.find("fedcba9876543210fedcba9876543210") == std::string::npos);
    CHECK(r.body.find("RT_NEW") == std::string::npos);
    CHECK(r.body.find("AT1") == std::string::npos);
    r = h.request("POST", base + "/log-clear");
    CHECK(r.status == 303);
    r = h.request("GET", base + "/log.txt");
    CHECK(r.body.find("credentials saved") == std::string::npos && r.body.find("log cleared") != std::string::npos);

    srv.stop();
    CHECK(!srv.running());
}

int main() {
    mkdir("sdmc:", 0777);
    curl_global_init(CURL_GLOBAL_DEFAULT);

    testUtil();
    testConfig();   // leaves a config with cid/csecret/RT1 for the remote test
    testRemote();
    testSetupServer();

    Http::abortAll(true);
    Remote::get().stop();
    curl_global_cleanup();

    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
