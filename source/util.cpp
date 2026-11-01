#include "util.hpp"

#include <cstdio>

namespace spr {

    std::string formatTime(u32 ms) {
        const u32 total = ms / 1000;
        const u32 h = total / 3600, m = (total / 60) % 60, s = total % 60;
        char buf[16];
        if (h > 0)
            snprintf(buf, sizeof(buf), "%u:%02u:%02u", h, m, s);
        else
            snprintf(buf, sizeof(buf), "%u:%02u", m, s);
        return buf;
    }

    std::string urlEncode(std::string_view in) {
        static constexpr char hex[] = "0123456789ABCDEF";
        std::string out;
        out.reserve(in.size() * 3);
        for (unsigned char c : in) {
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                c == '-' || c == '_' || c == '.' || c == '~') {
                out += static_cast<char>(c);
            } else {
                out += '%';
                out += hex[c >> 4];
                out += hex[c & 0xF];
            }
        }
        return out;
    }

    static int hexValue(char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    }

    std::string urlDecode(std::string_view in) {
        std::string out;
        out.reserve(in.size());
        for (size_t i = 0; i < in.size(); ++i) {
            const char c = in[i];
            if (c == '+') {
                out += ' ';
            } else if (c == '%' && i + 2 < in.size() && hexValue(in[i + 1]) >= 0 && hexValue(in[i + 2]) >= 0) {
                out += static_cast<char>(hexValue(in[i + 1]) * 16 + hexValue(in[i + 2]));
                i += 2;
            } else {
                out += c;
            }
        }
        return out;
    }

    std::string base64Encode(std::string_view in) {
        static constexpr char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string out;
        out.reserve(((in.size() + 2) / 3) * 4);
        size_t i = 0;
        for (; i + 2 < in.size(); i += 3) {
            const u32 n = (u8(in[i]) << 16) | (u8(in[i + 1]) << 8) | u8(in[i + 2]);
            out += tbl[(n >> 18) & 63];
            out += tbl[(n >> 12) & 63];
            out += tbl[(n >> 6) & 63];
            out += tbl[n & 63];
        }
        if (i + 1 == in.size()) {
            const u32 n = u8(in[i]) << 16;
            out += tbl[(n >> 18) & 63];
            out += tbl[(n >> 12) & 63];
            out += "==";
        } else if (i + 2 == in.size()) {
            const u32 n = (u8(in[i]) << 16) | (u8(in[i + 1]) << 8);
            out += tbl[(n >> 18) & 63];
            out += tbl[(n >> 12) & 63];
            out += tbl[(n >> 6) & 63];
            out += '=';
        }
        return out;
    }

    std::string htmlEscape(std::string_view in) {
        std::string out;
        out.reserve(in.size());
        for (char c : in) {
            switch (c) {
                case '&': out += "&amp;"; break;
                case '<': out += "&lt;"; break;
                case '>': out += "&gt;"; break;
                case '"': out += "&quot;"; break;
                case '\'': out += "&#39;"; break;
                default: out += c;
            }
        }
        return out;
    }

    std::string jsonEscape(std::string_view in) {
        std::string out;
        out.reserve(in.size() + 2);
        for (unsigned char c : in) {
            switch (c) {
                case '"': out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default:
                    if (c < 0x20) {
                        char buf[8];
                        snprintf(buf, sizeof(buf), "\\u%04x", c);
                        out += buf;
                    } else {
                        out += static_cast<char>(c);
                    }
            }
        }
        return out;
    }

    std::string queryParam(std::string_view query, std::string_view key) {
        size_t pos = 0;
        while (pos <= query.size()) {
            size_t end = query.find('&', pos);
            if (end == std::string_view::npos) end = query.size();
            const std::string_view pair = query.substr(pos, end - pos);
            const size_t eq = pair.find('=');
            if (pair.substr(0, eq) == key)
                return eq == std::string_view::npos ? std::string() : urlDecode(pair.substr(eq + 1));
            pos = end + 1;
        }
        return {};
    }

    std::string trim(std::string_view s) {
        const size_t b = s.find_first_not_of(" \t\r\n");
        if (b == std::string_view::npos) return {};
        const size_t e = s.find_last_not_of(" \t\r\n");
        return std::string(s.substr(b, e - b + 1));
    }

    std::string joinArtists(const cJSON* artists) {
        std::string out;
        if (!artists || !cJSON_IsArray(artists)) return out;
        const cJSON* a = nullptr;
        cJSON_ArrayForEach(a, artists) {
            const std::string name = jstr(a, "name");
            if (name.empty()) continue;
            if (!out.empty()) out += ", ";
            out += name;
        }
        return out;
    }

}
