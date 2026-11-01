#include "http.hpp"

#include <cstdlib>
#include <cstring>
#include <strings.h>

namespace spr {

    // Spotify payloads we use are well under this; the cap protects the small overlay heap.
    static constexpr size_t MAX_BODY_SIZE = 512 * 1024;

    struct WriteCtx {
        std::string* body;
        bool overflow;
    };

    static size_t writeCb(char* ptr, size_t size, size_t nmemb, void* userdata) {
        auto* ctx = static_cast<WriteCtx*>(userdata);
        const size_t n = size * nmemb;
        if (ctx->body->size() + n > MAX_BODY_SIZE) {
            ctx->overflow = true;
            return 0;  // aborts the transfer
        }
        ctx->body->append(ptr, n);
        return n;
    }

    static size_t headerCb(char* buffer, size_t size, size_t nitems, void* userdata) {
        const size_t n = size * nitems;
        static constexpr char key[] = "retry-after:";
        if (n > sizeof(key) - 1 && strncasecmp(buffer, key, sizeof(key) - 1) == 0) {
            std::string v(buffer + sizeof(key) - 1, n - (sizeof(key) - 1));
            *static_cast<int*>(userdata) = atoi(v.c_str());
        }
        return n;
    }

    static int progressCb(void* abortFlag, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
        return static_cast<std::atomic<bool>*>(abortFlag)->load() ? 1 : 0;
    }

    Http::Http() = default;

    Http::~Http() {
        close();
    }

    void Http::close() {
        if (m_curl) curl_easy_cleanup(m_curl);
        m_curl = nullptr;
    }

    HttpResponse Http::request(const char* method, const std::string& url,
                               const std::vector<std::string>& headers, const std::string& body) {
        HttpResponse res;
        if (!m_curl && !s_abort) m_curl = curl_easy_init();
        if (!m_curl || s_abort) {
            res.curlCode = m_curl ? CURLE_ABORTED_BY_CALLBACK : CURLE_FAILED_INIT;
            res.error = m_curl ? "aborted" : "curl init failed";
            return res;
        }

        // Resets options but keeps the connection cache.
        curl_easy_reset(m_curl);

        WriteCtx ctx{&res.body, false};
        char errbuf[CURL_ERROR_SIZE] = {};

        curl_slist* hdrs = nullptr;
        for (const auto& h : headers) hdrs = curl_slist_append(hdrs, h.c_str());

        curl_easy_setopt(m_curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(m_curl, CURLOPT_HTTPHEADER, hdrs);
        curl_easy_setopt(m_curl, CURLOPT_WRITEFUNCTION, writeCb);
        curl_easy_setopt(m_curl, CURLOPT_WRITEDATA, &ctx);
        curl_easy_setopt(m_curl, CURLOPT_HEADERFUNCTION, headerCb);
        curl_easy_setopt(m_curl, CURLOPT_HEADERDATA, &res.retryAfterSec);
        curl_easy_setopt(m_curl, CURLOPT_ERRORBUFFER, errbuf);
        curl_easy_setopt(m_curl, CURLOPT_USERAGENT, "SpotifyRemoteOverlay/1.0 (Nintendo Switch)");
        curl_easy_setopt(m_curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
        curl_easy_setopt(m_curl, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
        curl_easy_setopt(m_curl, CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_2);
        curl_easy_setopt(m_curl, CURLOPT_SSL_VERIFYPEER, m_verify ? 1L : 0L);
        curl_easy_setopt(m_curl, CURLOPT_SSL_VERIFYHOST, m_verify ? 2L : 0L);
        curl_easy_setopt(m_curl, CURLOPT_CONNECTTIMEOUT, 6L);
        curl_easy_setopt(m_curl, CURLOPT_TIMEOUT, 12L);
        curl_easy_setopt(m_curl, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(m_curl, CURLOPT_BUFFERSIZE, 16384L);
        curl_easy_setopt(m_curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(m_curl, CURLOPT_XFERINFOFUNCTION, progressCb);
        curl_easy_setopt(m_curl, CURLOPT_XFERINFODATA, &s_abort);

        if (strcmp(method, "GET") == 0) {
            curl_easy_setopt(m_curl, CURLOPT_HTTPGET, 1L);
        } else {
            curl_easy_setopt(m_curl, CURLOPT_POSTFIELDS, body.c_str());
            curl_easy_setopt(m_curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
            if (strcmp(method, "POST") != 0)
                curl_easy_setopt(m_curl, CURLOPT_CUSTOMREQUEST, method);
        }

        res.curlCode = curl_easy_perform(m_curl);
        curl_easy_getinfo(m_curl, CURLINFO_RESPONSE_CODE, &res.status);
        curl_slist_free_all(hdrs);

        if (res.curlCode != CURLE_OK) {
            if (ctx.overflow)
                res.error = "response too large";
            else
                res.error = errbuf[0] ? errbuf : curl_easy_strerror(res.curlCode);
        }
        return res;
    }

}
