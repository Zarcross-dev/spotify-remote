#pragma once

#include <curl/curl.h>

#include <atomic>
#include <string>
#include <vector>

namespace spr {

    struct HttpResponse {
        CURLcode curlCode = CURLE_OK;
        long status       = 0;
        int retryAfterSec = 0;     // from a 429 Retry-After header
        std::string body;
        std::string error;         // transport error text, empty on success

        bool transportOk() const { return curlCode == CURLE_OK; }
        bool ok() const          { return transportOk() && status >= 200 && status < 300; }
    };

    // One curl easy handle per thread. Reusing it keeps the TLS connection to
    // api.spotify.com alive between calls, which matters a lot on the Switch.
    class Http {
    public:
        Http();
        ~Http();
        Http(const Http&) = delete;
        Http& operator=(const Http&) = delete;

        void setVerifyPeer(bool verify) { m_verify = verify; }

        // Makes in-flight and future requests fail fast; used on overlay exit
        // so closing never waits for a slow network round trip.
        static void abortAll(bool abort) { s_abort = abort; }

        // Frees the curl handle (and its TLS connection). Must run before
        // curl_global_cleanup()/socketExit(): freeing it later, e.g. from a static
        // destructor at process exit, talks to already closed services and crashes.
        // The handle is recreated on the next request.
        void close();

        // `body` is sent for every method except GET (empty body => Content-Length: 0,
        // which Spotify requires on bodiless PUT/POST).
        HttpResponse request(const char* method, const std::string& url,
                             const std::vector<std::string>& headers = {},
                             const std::string& body = {});

    private:
        static inline std::atomic<bool> s_abort{false};

        CURL* m_curl = nullptr;
        bool m_verify = true;
    };

}
