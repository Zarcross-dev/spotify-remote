#pragma once

#include <switch.h>

#include <atomic>
#include <mutex>
#include <string>

namespace spr {

    // Tiny HTTP server that runs on the Switch while the Setup screen is open.
    // From any browser on the same network the user enters the client id and
    // secret, authorizes with Spotify, and pastes back the redirect URL; the
    // Switch then exchanges the code for a refresh token and saves config.ini.
    class SetupServer {
    public:
        static constexpr u16 PORT = 8888;

        static SetupServer& get();

        bool start();
        void stop();
        bool running() const { return m_running; }

        // http://<switch ip>:8888/<key>/ - the random key keeps other devices on
        // the network from reaching the setup pages without looking at the screen.
        std::string url() const;
        std::string status();        // last thing that happened, for the overlay

    private:
        SetupServer() = default;

        static void threadEntry(void* arg);
        void threadMain();
        void handleClient(int fd, const std::string& peer);
        void setStatus(const std::string& s);

        std::string pageForm(const std::string& notice);
        std::string pageAuthorize(const std::string& clientId);
        std::string pageDone();
        std::string pageError(const std::string& msg);
        std::string pageLog();

        int m_listenFd = -1;
        Thread m_thread{};
        bool m_running = false;
        std::atomic<bool> m_stop{false};

        std::mutex m_mutex;
        std::string m_status;
        std::string m_state;        // OAuth "state" for the authorization in flight
        std::string m_key;          // per-start access key, part of every URL
    };

}
