#include "setup_server.hpp"
#include "config.hpp"
#include "http.hpp"
#include "log.hpp"
#include "remote.hpp"
#include "spotify.hpp"
#include "util.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <strings.h>
#include <chrono>
#include <thread>

namespace spr {

    static constexpr size_t MAX_REQUEST     = 16 * 1024;
    static constexpr u64 REQUEST_DEADLINE   = 4000;  // whole request, so a slow client can't stall the server

    static std::string makeKey() {
        // No look-alike characters, it gets typed on a phone.
        static constexpr char alphabet[] = "abcdefghjkmnpqrstuvwxyz23456789";
        std::string key;
        for (int i = 0; i < 6; ++i) key += alphabet[randomGet64() % (sizeof(alphabet) - 1)];
        return key;
    }

    // Spotify client ids/secrets are 32 hex chars; accept any alnum token of sane length.
    static bool validCredential(const std::string& s) {
        if (s.size() < 16 || s.size() > 64) return false;
        for (unsigned char c : s)
            if (!isalnum(c)) return false;
        return true;
    }

    SetupServer& SetupServer::get() {
        static SetupServer instance;
        return instance;
    }

    std::string SetupServer::url() const {
        u32 ip = 0;
        if (R_FAILED(nifmGetCurrentIpAddress(&ip)) || ip == 0) return {};
        char buf[40];
        snprintf(buf, sizeof(buf), "http://%u.%u.%u.%u:%u/",
                 ip & 0xFF, (ip >> 8) & 0xFF, (ip >> 16) & 0xFF, (ip >> 24) & 0xFF, PORT);
        return std::string(buf) + m_key + "/";
    }

    std::string SetupServer::status() {
        std::lock_guard lock(m_mutex);
        return m_status;
    }

    void SetupServer::setStatus(const std::string& s) {
        std::lock_guard lock(m_mutex);
        m_status = s;
    }

    bool SetupServer::start() {
        if (m_running) return true;

        m_listenFd = socket(AF_INET, SOCK_STREAM, 0);
        if (m_listenFd < 0) {
            setStatus("Could not create socket");
            return false;
        }
        const int yes = 1;
        setsockopt(m_listenFd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(PORT);
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        if (bind(m_listenFd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0 || listen(m_listenFd, 4) < 0) {
            close(m_listenFd);
            m_listenFd = -1;
            setStatus("Port 8888 is busy");
            logf("setup server: bind/listen on port %u failed (errno %d)", PORT, errno);
            return false;
        }

        m_stop = false;
        m_key = makeKey();
        {
            std::lock_guard lock(m_mutex);
            m_state.clear();
        }
        if (R_FAILED(threadCreate(&m_thread, &SetupServer::threadEntry, this, nullptr, 0x20000, 0x2C, -2)) ||
            R_FAILED(threadStart(&m_thread))) {
            close(m_listenFd);
            m_listenFd = -1;
            setStatus("Could not start server thread");
            return false;
        }

        m_running = true;
        setStatus("Waiting for a browser...");
        logf("setup server listening on port %u", PORT);
        return true;
    }

    void SetupServer::stop() {
        if (!m_running) return;
        m_stop = true;
        threadWaitForExit(&m_thread);
        threadClose(&m_thread);
        close(m_listenFd);
        m_listenFd = -1;
        m_running = false;
        {
            std::lock_guard lock(m_mutex);
            m_state.clear();
        }
        logf("setup server stopped");
    }

    void SetupServer::threadEntry(void* arg) {
        static_cast<SetupServer*>(arg)->threadMain();
    }

    void SetupServer::threadMain() {
        while (!m_stop) {
            pollfd pfd{m_listenFd, POLLIN, 0};
            if (poll(&pfd, 1, 250) <= 0 || !(pfd.revents & POLLIN)) continue;

            sockaddr_in peer{};
            socklen_t peerLen = sizeof(peer);
            const int fd = accept(m_listenFd, reinterpret_cast<sockaddr*>(&peer), &peerLen);
            if (fd < 0) continue;
            char ip[16] = "?";  // "255.255.255.255"
            inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
            handleClient(fd, ip);
            close(fd);
        }
    }

    // ------------------------------------------------------------------
    // HTTP plumbing
    // ------------------------------------------------------------------

    static void sendAll(int fd, const std::string& data) {
        size_t off = 0;
        while (off < data.size()) {
            const ssize_t n = send(fd, data.data() + off, data.size() - off, 0);
            if (n <= 0) return;
            off += static_cast<size_t>(n);
        }
    }

    static void respond(int fd, const char* status, const std::string& content,
                        const char* type = "text/html; charset=utf-8", const std::string& extraHeaders = {}) {
        // no-referrer: the key in our URLs must not leak to Spotify via the Referer header.
        std::string head = std::string("HTTP/1.1 ") + status + "\r\n"
                           "Content-Type: " + type + "\r\n"
                           "Cache-Control: no-store\r\n"
                           "X-Content-Type-Options: nosniff\r\n"
                           "X-Frame-Options: DENY\r\n"
                           "Referrer-Policy: no-referrer\r\n"
                           "Connection: close\r\n" + extraHeaders +
                           "Content-Length: " + std::to_string(content.size()) + "\r\n\r\n";
        sendAll(fd, head + content);
    }

    static bool readRequest(int fd, std::string& method, std::string& target, std::string& body) {
        timeval tv{1, 0};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        const u64 deadline = nowMs() + REQUEST_DEADLINE;

        std::string req;
        char buf[2048];
        size_t headerEnd = std::string::npos;
        size_t contentLength = 0;

        while (req.size() < MAX_REQUEST && nowMs() < deadline) {
            const ssize_t n = recv(fd, buf, sizeof(buf), 0);
            if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) break;
            if (n < 0) continue;
            req.append(buf, static_cast<size_t>(n));

            if (headerEnd == std::string::npos) {
                headerEnd = req.find("\r\n\r\n");
                if (headerEnd == std::string::npos) continue;
                // Content-Length, case-insensitive
                for (size_t pos = 0; pos < headerEnd;) {
                    const size_t eol = req.find("\r\n", pos);
                    const std::string line = req.substr(pos, eol - pos);
                    if (line.size() > 15 && strncasecmp(line.c_str(), "content-length:", 15) == 0) {
                        const long n = atol(line.c_str() + 15);
                        contentLength = n > 0 ? std::min<size_t>(static_cast<size_t>(n), MAX_REQUEST) : 0;
                    }
                    pos = eol + 2;
                }
            }
            if (headerEnd != std::string::npos && req.size() >= headerEnd + 4 + contentLength) break;
        }
        if (headerEnd == std::string::npos) return false;

        const size_t sp1 = req.find(' ');
        const size_t sp2 = req.find(' ', sp1 + 1);
        if (sp1 == std::string::npos || sp2 == std::string::npos) return false;
        method = req.substr(0, sp1);
        target = req.substr(sp1 + 1, sp2 - sp1 - 1);
        body = req.substr(headerEnd + 4, contentLength);
        return true;
    }

    // ------------------------------------------------------------------
    // Pages
    // ------------------------------------------------------------------

    static std::string page(const std::string& content) {
        return
            "<!doctype html><html><head><meta charset=utf-8>"
            "<meta name=viewport content='width=device-width,initial-scale=1'>"
            "<title>Spotify Remote setup</title><style>"
            "body{font-family:system-ui,sans-serif;background:#121212;color:#eee;margin:0;padding:16px;line-height:1.5}"
            ".c{max-width:560px;margin:auto}h1{color:#1db954;font-size:1.5em}"
            "label{display:block;margin-top:14px;font-weight:600}"
            "input{width:100%;box-sizing:border-box;padding:10px;margin-top:4px;border-radius:6px;border:1px solid #444;background:#222;color:#eee;font-size:1em}"
            "button,.btn{display:inline-block;margin-top:16px;padding:12px 20px;border:0;border-radius:24px;background:#1db954;color:#000;font-weight:700;font-size:1em;text-decoration:none;cursor:pointer}"
            "code{background:#222;padding:2px 6px;border-radius:4px;word-break:break-all}"
            ".box{background:#1c1c1c;border-radius:8px;padding:12px 16px;margin:12px 0}"
            ".err{background:#4a1515;border-radius:8px;padding:10px 14px}.ok{color:#1db954}"
            "ol{padding-left:20px}</style></head><body><div class=c>"
            "<h1>Spotify Remote setup</h1>" + content +
            "<p style='margin-top:32px;font-size:.85em'><a style='color:#888' href='log'>Debug log</a></p>"
            "</div></body></html>";
    }

    std::string SetupServer::pageForm(const std::string& notice) {
        Config cfg;
        cfg.load();
        std::string html;
        if (!notice.empty()) html += "<p class=err>" + htmlEscape(notice) + "</p>";
        html +=
            "<div class=box><b>Step 1 - create a Spotify app</b><ol>"
            "<li>Open <a style='color:#1db954' href='https://developer.spotify.com/dashboard' target=_blank rel=noreferrer>developer.spotify.com/dashboard</a> and create an app (select <i>Web API</i>).</li>"
            "<li>Add this Redirect URI exactly:<br><code>" + std::string(REDIRECT_URI) + "</code></li>"
            "<li>Copy the app's Client ID and Client Secret below.</li></ol></div>"
            "<form method=post action=save>"
            "<label>Client ID<input name=client_id autocomplete=off required value='" + htmlEscape(cfg.clientId) + "'></label>"
            "<label>Client Secret<input name=client_secret autocomplete=off type=password placeholder='" +
            std::string(cfg.clientSecret.empty() ? "" : "(saved - leave empty to keep)") + "'></label>"
            "<button>Save and continue</button></form>";
        return page(html);
    }

    std::string SetupServer::pageAuthorize(const std::string& clientId) {
        char state[33];
        snprintf(state, sizeof(state), "%016llx%016llx",
                 static_cast<unsigned long long>(randomGet64()), static_cast<unsigned long long>(randomGet64()));
        {
            std::lock_guard lock(m_mutex);
            m_state = state;
        }

        const std::string authUrl =
            "https://accounts.spotify.com/authorize?response_type=code&show_dialog=true"
            "&client_id=" + urlEncode(clientId) +
            "&redirect_uri=" + urlEncode(REDIRECT_URI) +
            "&scope=" + urlEncode(SCOPES) +
            "&state=" + std::string(state);

        return page(
            "<div class=box><b>Step 2 - log in to Spotify</b><p>Tap the button, log in and accept. "
            "You will land on a page that <b>fails to load</b> (127.0.0.1) - that is expected.</p>"
            "<a class=btn href='" + htmlEscape(authUrl) + "' target=_blank rel=noreferrer>Log in with Spotify</a></div>"
            "<div class=box><b>Step 3 - paste the address</b><p>Copy the full address from the browser's address bar "
            "(it starts with <code>http://127.0.0.1:8888/callback?code=</code>) and paste it here.</p>"
            "<form method=post action=code><input name=url autocomplete=off required placeholder='http://127.0.0.1:8888/callback?code=...'>"
            "<button>Finish setup</button></form></div>");
    }

    std::string SetupServer::pageDone() {
        return page("<p class=ok><b>All set!</b></p><p>The Switch is now linked to your Spotify account. "
                    "Start playing something on any device and control it from the overlay.</p>");
    }

    std::string SetupServer::pageError(const std::string& msg) {
        return page("<p class=err>" + htmlEscape(msg) + "</p><a class=btn href='./'>Start over</a>");
    }

    std::string SetupServer::pageLog() {
        std::string lines;
        for (const auto& l : logLines()) lines += htmlEscape(l) + "\n";
        if (lines.empty()) lines = "(empty)";
        return
            "<!doctype html><html><head><meta charset=utf-8>"
            "<meta name=viewport content='width=device-width,initial-scale=1'>"
            "<title>Spotify Remote log</title><style>"
            "body{font-family:system-ui,sans-serif;background:#121212;color:#eee;margin:0;padding:16px}"
            "pre{background:#1c1c1c;padding:12px;border-radius:8px;font-size:12px;white-space:pre-wrap;word-break:break-all}"
            "a,button{color:#1db954;margin-right:16px}button{background:none;border:0;font-size:1em;cursor:pointer;padding:0}"
            "</style></head><body>"
            "<p><a href='./'>Setup</a><a href='log.txt'>Plain text</a><a id=t href='#'>Pause</a>"
            "<form method=post action=log-clear style='display:inline'><button>Clear</button></form></p>"
            "<pre id=l>" + lines + "</pre>"
            "<script>"
            "let paused=false;const t=document.getElementById('t'),l=document.getElementById('l');"
            "t.onclick=e=>{e.preventDefault();paused=!paused;t.textContent=paused?'Resume':'Pause'};"
            "let stick=true;window.onscroll=()=>{stick=innerHeight+scrollY>=document.body.scrollHeight-40};"
            "async function tick(){if(!paused){try{const r=await fetch('log.txt',{cache:'no-store'});"
            "l.textContent=await r.text()||'(empty)';if(stick)scrollTo(0,document.body.scrollHeight)}catch(e){}}"
            "setTimeout(tick,1500)}scrollTo(0,document.body.scrollHeight);setTimeout(tick,1500);"
            "</script></body></html>";
    }

    // ------------------------------------------------------------------
    // Routes
    // ------------------------------------------------------------------

    void SetupServer::handleClient(int fd, const std::string& peer) {
        std::string method, target, body;
        if (!readRequest(fd, method, target, body)) {
            logf("web %s: bad or timed out request", peer.c_str());
            return;
        }

        const size_t q = target.find('?');
        std::string path = target.substr(0, q);

        // Every page lives under /<key>/. Anything else gets a hint, slowly,
        // so the key can't be brute forced.
        const std::string prefix = "/" + m_key;
        if (path == prefix) {
            respond(fd, "301 Moved Permanently", "", "text/plain", "Location: " + prefix + "/\r\n");
            return;
        }
        if (path.compare(0, prefix.size() + 1, prefix + "/") != 0) {
            logf("web %s: request without the access key", peer.c_str());
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            respond(fd, "403 Forbidden", page("<p class=err>Use the exact address shown on the Switch's Setup screen "
                                              "(it ends with a short code).</p>"));
            return;
        }
        path = path.substr(prefix.size());  // "/", "/save", ...

        // The log viewer polls itself; don't flood the log with its own requests.
        if (method == "GET" && path == "/log.txt") {
            std::string text;
            for (const auto& l : logLines()) text += l + "\n";
            respond(fd, "200 OK", text, "text/plain; charset=utf-8");
            return;
        }
        if (method == "GET" && path == "/log") {
            respond(fd, "200 OK", pageLog());
            return;
        }
        if (method == "POST" && path == "/log-clear") {
            logClear();
            logf("log cleared from %s", peer.c_str());
            respond(fd, "303 See Other", "", "text/plain", "Location: log\r\n");
            return;
        }

        logf("web %s: %s %s (%zu byte body)", peer.c_str(), method.c_str(), path.c_str(), body.size());

        if (method == "GET" && path == "/") {
            setStatus("Browser connected");
            respond(fd, "200 OK", pageForm(""));
            return;
        }

        if (method == "POST" && path == "/save") {
            const std::string id = trim(queryParam(body, "client_id"));
            const std::string secret = trim(queryParam(body, "client_secret"));

            Config current;
            current.load();
            if (!validCredential(id) || (secret.empty() ? current.clientSecret.empty() : !validCredential(secret))) {
                logf("credentials rejected: invalid client id or secret format");
                respond(fd, "200 OK", pageForm("Client ID and Client Secret must be copied exactly from the Spotify dashboard "
                                               "(32 letters and digits each)."));
                return;
            }

            Config saved;
            Config::update([&](Config& c) {
                if (id != c.clientId) c.refreshToken.clear();  // token belongs to the old app
                c.clientId = id;
                if (!secret.empty()) c.clientSecret = secret;
                saved = c;
            });

            logf("credentials saved: client_id=%s secret=%s", redact(saved.clientId).c_str(),
                 secret.empty() ? "kept previous" : "updated");
            Remote::get().post({Cmd::ReloadConfig});
            setStatus("Credentials saved, waiting for Spotify login...");
            respond(fd, "200 OK", pageAuthorize(saved.clientId));
            return;
        }

        if (method == "POST" && path == "/code") {
            // The pasted redirect URL (or just its query string).
            const std::string input = trim(queryParam(body, "url"));
            const size_t qm = input.find('?');
            const std::string params = qm == std::string::npos ? input : input.substr(qm + 1);

            const std::string code = queryParam(params, "code");
            const std::string state = queryParam(params, "state");
            const std::string err = queryParam(params, "error");

            logf("login callback: code=%s state=%s error=%s", code.empty() ? "missing" : "present",
                 state.empty() ? "missing" : "present", err.empty() ? "none" : err.c_str());
            if (!err.empty()) {
                setStatus("Spotify login was cancelled");
                respond(fd, "200 OK", pageError("Spotify returned: " + err));
                return;
            }
            {
                // The state must match the login link we generated, so only a login
                // started from this page can be completed (no injected codes).
                std::lock_guard lock(m_mutex);
                if (code.empty() || m_state.empty() || state != m_state) {
                    logf("login rejected: %s", code.empty() ? "no code in pasted text" : "state mismatch (old login link?)");
                    respond(fd, "200 OK", pageError("That address doesn't contain a valid login code. Make sure you copy the newest one."));
                    return;
                }
                m_state.clear();  // one use only
            }

            Config cfg;
            cfg.load();
            if (!cfg.hasCredentials()) {
                respond(fd, "200 OK", pageError("Client ID / Secret are missing."));
                return;
            }

            setStatus("Exchanging login code...");
            Http http;
            http.setVerifyPeer(cfg.sslVerify);
            std::string refreshToken, error;
            if (!SpotifyApi::exchangeCode(http, cfg.clientId, cfg.clientSecret, code, refreshToken, error)) {
                setStatus("Login failed: " + error);
                respond(fd, "200 OK", pageError("Login failed: " + error));
                return;
            }

            Config::update([&](Config& c) { c.refreshToken = refreshToken; });
            logf("refresh token saved to config.ini, account linked");
            Remote::get().post({Cmd::ReloadConfig});
            setStatus("Linked! You can close the browser.");
            respond(fd, "200 OK", pageDone());
            return;
        }

        logf("web %s: 404 for %s", peer.c_str(), path.c_str());
        respond(fd, "404 Not Found", page("<p>Not found. <a style='color:#1db954' href='./'>Go to setup</a></p>"));
    }

}
