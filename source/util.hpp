#pragma once

#include <switch.h>
#include <cJSON.h>

#include <string>
#include <string_view>

namespace spr {

    inline u64 nowMs() {
        return armTicksToNs(armGetSystemTick()) / 1000000ULL;
    }

    // "m:ss" or "h:mm:ss"
    std::string formatTime(u32 ms);

    std::string urlEncode(std::string_view in);
    std::string urlDecode(std::string_view in);
    std::string base64Encode(std::string_view in);
    std::string htmlEscape(std::string_view in);
    std::string jsonEscape(std::string_view in);

    // Value of `key` in an "a=1&b=2" string (URL-decoded), empty if missing.
    std::string queryParam(std::string_view query, std::string_view key);

    std::string trim(std::string_view s);

    // Number of bytes of the UTF-8 sequence starting with `lead`.
    inline size_t utf8Len(unsigned char lead) {
        if (lead < 0x80) return 1;
        if ((lead >> 5) == 0x6) return 2;
        if ((lead >> 4) == 0xE) return 3;
        if ((lead >> 3) == 0x1E) return 4;
        return 1;
    }

    // ---- cJSON helpers (null-safe) ----
    inline const cJSON* jget(const cJSON* obj, const char* key) {
        return obj ? cJSON_GetObjectItemCaseSensitive(obj, key) : nullptr;
    }
    inline std::string jstr(const cJSON* obj, const char* key, const char* def = "") {
        const cJSON* v = jget(obj, key);
        return (v && cJSON_IsString(v) && v->valuestring) ? v->valuestring : def;
    }
    inline long long jnum(const cJSON* obj, const char* key, long long def = 0) {
        const cJSON* v = jget(obj, key);
        return (v && cJSON_IsNumber(v)) ? static_cast<long long>(v->valuedouble) : def;
    }
    inline bool jbool(const cJSON* obj, const char* key, bool def = false) {
        const cJSON* v = jget(obj, key);
        return (v && cJSON_IsBool(v)) ? cJSON_IsTrue(v) : def;
    }

    // "Artist A, Artist B" from an array of artist objects.
    std::string joinArtists(const cJSON* artists);

    // RAII owner for a parsed cJSON tree.
    class Json {
    public:
        explicit Json(const std::string& text) : m_root(cJSON_ParseWithLength(text.data(), text.size())) {}
        ~Json() { if (m_root) cJSON_Delete(m_root); }
        Json(const Json&) = delete;
        Json& operator=(const Json&) = delete;

        const cJSON* root() const { return m_root; }
        explicit operator bool() const { return m_root != nullptr; }

    private:
        cJSON* m_root;
    };

}
