#include "protocol.h"
#include <algorithm>
#include <map>

namespace protocol {
static std::string_view Trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    return s;
}
static std::string Lower(std::string_view s) {
    std::string r(s);
    for (char& c : r) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    return r;
}
static bool Token(std::string_view s, std::string_view wanted) {
    while (!s.empty()) {
        auto n = s.find(',');
        if (Lower(Trim(s.substr(0, n))) == wanted) return true;
        if (n == s.npos) break;
        s.remove_prefix(n + 1);
    }
    return false;
}
std::string Base64(const unsigned char* data, std::size_t size) {
    const char* chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (std::size_t i = 0; i < size; i += 3) {
        std::uint32_t v = std::uint32_t(data[i]) << 16;
        if (i + 1 < size) v |= std::uint32_t(data[i + 1]) << 8;
        if (i + 2 < size) v |= data[i + 2];
        out += chars[v >> 18]; out += chars[(v >> 12) & 63];
        out += i + 1 < size ? chars[(v >> 6) & 63] : '=';
        out += i + 2 < size ? chars[v & 63] : '=';
    }
    return out;
}
Handshake CheckHandshake(std::string_view h) {
    if (h.size() > MaxHeader) return {431, {}};
    if (h.size() < 4 || h.substr(h.size() - 4) != "\r\n\r\n") return {};
    auto line = h.find("\r\n");
    // Companion always requests /. No general HTTP routes are exposed.
    if (h.substr(0, line) != "GET / HTTP/1.1") return {};
    h.remove_prefix(line + 2);
    std::map<std::string, std::string> fields;
    while (h != "\r\n") {
        line = h.find("\r\n");
        if (line == h.npos) return {};
        auto row = h.substr(0, line);
        auto colon = row.find(':');
        if (colon == row.npos || colon == 0) return {};
        std::string name = Lower(row.substr(0, colon));
        for (unsigned char c : name) {
            if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                std::string_view("!#$%&'*+-.^_`|~").find(c) != std::string_view::npos)) return {};
        }
        auto value = Trim(row.substr(colon + 1));
        for (unsigned char c : value) if (c == 0x7f || (c < 32 && c != '\t')) return {};
        if (!fields.emplace(name, value).second) return {};
        h.remove_prefix(line + 2);
    }
    // Blocks browser-originated drive-by control. This is not authentication.
    if (fields.count("origin")) return {403, {}};
    if (!fields.count("host") || fields["host"].empty() ||
        Lower(fields["upgrade"]) != "websocket" || !Token(fields["connection"], "upgrade") ||
        fields["sec-websocket-version"] != "13") return {426, {}};
    if (fields.count("transfer-encoding") ||
        (fields.count("content-length") && fields["content-length"] != "0")) return {};
    const auto& key = fields["sec-websocket-key"];
    const std::string chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    if (key.size() != 24 || key.substr(22) != "==") return {};
    for (std::size_t i = 0; i < 22; ++i) if (chars.find(key[i]) == std::string::npos) return {};
    if ((chars.find(key[21]) & 15) != 0) return {}; // Canonical 16-byte nonce.
    return {101, key};
}
bool ValidUtf8(std::string_view s) {
    for (std::size_t i = 0; i < s.size();) {
        unsigned c = static_cast<unsigned char>(s[i++]);
        if (c < 128) continue;
        unsigned n, cp, min;
        if (c >= 0xc2 && c <= 0xdf) { n = 1; cp = c & 31; min = 0x80; }
        else if (c >= 0xe0 && c <= 0xef) { n = 2; cp = c & 15; min = 0x800; }
        else if (c >= 0xf0 && c <= 0xf4) { n = 3; cp = c & 7; min = 0x10000; }
        else return false;
        if (s.size() - i < n) return false;
        while (n--) {
            c = static_cast<unsigned char>(s[i++]);
            if ((c & 0xc0) != 0x80) return false;
            cp = (cp << 6) | (c & 63);
        }
        if (cp < min || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) return false;
    }
    return true;
}
bool ValidJsonDepth(std::string_view s, unsigned maxDepth) {
    unsigned depth = 0; bool quoted = false, escaped = false;
    for (char c : s) {
        if (quoted) {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') quoted = false;
        } else if (c == '"') quoted = true;
        else if (c == '{' || c == '[') { if (++depth > maxDepth) return false; }
        else if (c == '}' || c == ']') { if (!depth) return false; --depth; }
    }
    return depth == 0 && !quoted;
}
static bool Number(std::string_view s, unsigned max, unsigned& n) {
    if (s.empty()) return false;
    n = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        n = n * 10 + unsigned(c - '0');
        if (n > max) return false;
    }
    return true;
}
bool ParseIPv4(std::string_view s, std::uint32_t& address) {
    address = 0;
    for (int i = 0; i < 4; ++i) {
        auto n = s.find('.'); unsigned octet;
        if ((i == 3) != (n == s.npos) || !Number(s.substr(0, n), 255, octet)) return false;
        address = (address << 8) | octet;
        if (i != 3) s.remove_prefix(n + 1);
    }
    return true;
}
static bool Rule(std::string_view s, std::uint32_t& address, std::uint32_t& mask) {
    s = Trim(s); auto slash = s.find('/'); unsigned prefix = 32;
    if (!ParseIPv4(s.substr(0, slash), address)) return false;
    if (slash != s.npos && !Number(s.substr(slash + 1), 32, prefix)) return false;
    mask = prefix ? 0xffffffffu << (32 - prefix) : 0;
    return true;
}
bool ValidAllowList(std::string_view list) {
    if (list.empty() || list.size() > 1023) return false;
    while (true) {
        auto n = list.find(','); std::uint32_t address, mask;
        if (!Rule(list.substr(0, n), address, mask)) return false;
        if (n == list.npos) return true;
        list.remove_prefix(n + 1);
    }
}
bool Allowed(std::uint32_t address, std::string_view list) {
    if (!ValidAllowList(list)) return false;
    while (true) {
        auto n = list.find(','); std::uint32_t rule, mask;
        Rule(list.substr(0, n), rule, mask);
        if ((address & mask) == (rule & mask)) return true;
        if (n == list.npos) return false;
        list.remove_prefix(n + 1);
    }
}
std::string Frame(std::string_view payload, unsigned opcode) {
    std::string f(1, static_cast<char>(0x80 | opcode));
    std::uint64_t size = payload.size();
    if (size < 126) f += static_cast<char>(size);
    else if (size <= 65535) { f += char(126); f += char(size >> 8); f += char(size); }
    else { f += char(127); for (int i = 7; i >= 0; --i) f += char(size >> (8 * i)); }
    f.append(payload); return f;
}
Result Decoder::Next(std::string& buffer, Message& message) {
    if (buffer.size() < 2) return Result::NeedMore;
    const auto* p = reinterpret_cast<const unsigned char*>(buffer.data());
    unsigned op = p[0] & 15, code = p[1] & 127;
    bool fin = p[0] & 128; bool control = op >= 8;
    if ((p[0] & 0x70) || !(p[1] & 128) ||
        (op != 0 && op != 1 && op != 8 && op != 9 && op != 10) ||
        (control && (!fin || code > 125)) ||
        (op == 0 && !fragmented_) || (op == 1 && fragmented_)) return Result::Error;
    std::uint64_t size = code; std::size_t head = 2;
    if (code >= 126) {
        unsigned ext = code == 126 ? 2 : 8;
        if (buffer.size() < head + ext) return Result::NeedMore;
        size = 0; for (unsigned i = 0; i < ext; ++i) size = (size << 8) | p[head++];
        if ((code == 126 && size < 126) || (code == 127 && (size < 65536 || (size >> 63)))) return Result::Error;
    }
    if (size > MaxMessage || (!control && fragments_.size() > MaxMessage - size)) {
        closeCode = 1009; return Result::Error;
    }
    if (buffer.size() < head + 4 + size) return Result::NeedMore;
    std::string data(static_cast<std::size_t>(size), 0);
    for (std::size_t i = 0; i < size; ++i) data[i] = char(p[head + 4 + i] ^ p[head + i % 4]);
    buffer.erase(0, head + 4 + static_cast<std::size_t>(size));
    if (op == 8) {
        if (data.size() == 1) return Result::Error;
        if (data.size() >= 2) {
            unsigned status = (static_cast<unsigned char>(data[0]) << 8) | static_cast<unsigned char>(data[1]);
            if (!((status >= 1000 && status <= 1014 && status != 1004 && status != 1005 && status != 1006) ||
                  (status >= 3000 && status <= 4999))) return Result::Error;
            if (!ValidUtf8(std::string_view(data).substr(2))) { closeCode = 1007; return Result::Error; }
        }
    }
    if (!control) {
        if (op == 0) { fragments_ += data; data.clear(); }
        else if (!fin) fragments_ = std::move(data);
        fragmented_ = !fin;
        if (!fin) return Result::Fragment;
        if (op == 0) data.swap(fragments_);
        if (!ValidUtf8(data)) { closeCode = 1007; return Result::Error; }
        op = 1;
    }
    message = {op, std::move(data)};
    return Result::Message;
}
}
