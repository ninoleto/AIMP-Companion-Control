#include "protocol.h"
#include "command_validation.h"
#include <iostream>
#include <stdexcept>
#include <random>
using namespace protocol;
int checks = 0;
void Check(bool ok, const char* name) { ++checks; if (!ok) throw std::runtime_error(name); }
std::string Masked(std::string_view data, unsigned opcode = 1, bool fin = true) {
    std::string out = Frame(data, opcode);
    out[0] = char((fin ? 0x80 : 0) | opcode);
    size_t head = (out[1] == 126) ? 4 : (out[1] == 127) ? 10 : 2;
    out[1] |= char(0x80);
    out.insert(head, "abcd");
    for (size_t i = 0; i < data.size(); ++i) out[head + 4 + i] ^= "abcd"[i % 4];
    return out;
}
int main() try {
    const std::string handshake = "GET / HTTP/1.1\r\nhOsT: 127.0.0.1:19122\r\nupgrade: WebSocket\r\nconnection: keep-alive, Upgrade\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n";
    Check(CheckHandshake(handshake).status == 101, "valid mixed-case handshake");
    auto h = handshake; h.insert(h.size() - 2, "Origin: https://example.com\r\n");
    Check(CheckHandshake(h).status == 403, "browser Origin blocked");
    h = handshake; h.insert(h.size() - 2, "Origin:\r\n");
    Check(CheckHandshake(h).status == 403, "empty Origin blocked");
    h = handshake; h.insert(h.size() - 2, "Host: other\r\n");
    Check(CheckHandshake(h).status == 400, "duplicate header blocked");
    h = handshake; h.replace(h.find("ZQ=="), 4, "ZR==");
    Check(CheckHandshake(h).status == 400, "nonce padding must be canonical");
    h = handshake; h.replace(h.find("13\r"), 2, "12");
    Check(CheckHandshake(h).status == 426, "version 13 required");
    h = handshake; h.replace(0, 3, "POST");
    Check(CheckHandshake(h).status == 400, "GET required");
    Check(CheckHandshake(std::string(8193, 'x')).status == 431, "header size cap");
    std::uint32_t ip;
    Check(ParseIPv4("192.168.1.5", ip) && ip == 0xc0a80105, "IPv4 parse");
    for (auto s : {"192.168.1.5evil", "256.0.0.1", "1.2.3", "1.2.3.4.5", "-1.2.3.4"}) Check(!ParseIPv4(s, ip), "strict IPv4");
    Check(ValidAllowList("127.0.0.1, 192.168.1.0/24"), "valid allowlist");
    for (auto s : {"", "1.2.3.4/24oops", "1.2.3.4/33", "1.2.3.4,", "1.2.3.4,,5.6.7.8", "1.2.3.4/"}) Check(!ValidAllowList(s), "invalid allowlist denied");
    Check(Allowed(0xc0a80105, "127.0.0.1,192.168.1.0/24"), "CIDR match");
    Check(!Allowed(0xc0a80205, "192.168.1.0/24"), "CIDR nonmatch");
    Check(!Allowed(0x7f000001, "127.0.0.1,invalid"), "invalid policy fails closed");
    Check(Allowed(0x01020304, "0.0.0.0/0"), "explicit /0 rule");
    Check(ValidUtf8(u8"Žćš你好🎧"), "valid Unicode");
    for (auto s : {std::string("\xc0\xaf",2), std::string("\xed\xa0\x80",3), std::string("\xf4\x90\x80\x80",4), std::string("\xe2\x82",2)}) Check(!ValidUtf8(s), "invalid UTF8 rejected");
    Check(ValidJsonDepth("{\"x\":\"[[[\\\"\"}"), "brackets inside JSON string");
    Check(!ValidJsonDepth(std::string(33,'[') + std::string(33,']')), "JSON depth limit");
    for (int size : {0, 1, 125, 126, 65535, 65536}) {
        Decoder decoder; Message m; std::string expected(size, 'z'), buffer = Masked(expected);
        Check(decoder.Next(buffer,m) == Result::Message && m.payload == expected && buffer.empty(), "payload length boundary");
    }
    {
        Decoder d; Message m; std::string wire = Masked("hello"), buffer;
        for (size_t i = 0; i < wire.size(); ++i) {
            buffer += wire[i]; auto r = d.Next(buffer,m);
            Check(r == (i + 1 == wire.size() ? Result::Message : Result::NeedMore), "fragmented TCP delivery");
        }
    }
    {
        Decoder d; Message m; std::string b = Masked("{\"action\":", 1, false) + Masked("ping", 9) + Masked("\"player/status\"}", 0);
        Check(d.Next(b,m) == Result::Fragment, "start fragmented message");
        Check(d.Next(b,m) == Result::Message && m.opcode == 9 && m.payload == "ping", "interleaved ping remains control");
        Check(d.Next(b,m) == Result::Message && m.opcode == 1 && m.payload == "{\"action\":\"player/status\"}", "reassemble text");
    }
    auto Reject = [&](std::string buffer, unsigned code = 1002) {
        Decoder d; Message m; Check(d.Next(buffer,m) == Result::Error && d.closeCode == code, "invalid frame rejected");
    };
    Reject(Frame("unmasked")); Reject(Masked("binary",2)); Reject(Masked("continuation",0));
    Reject(Masked("ping",9,false)); Reject(Masked(std::string(126,'x'),9)); Reject(Masked("x",8));
    Reject(Masked(std::string("\x03\xed",2),8)); Reject(Masked(std::string("\xc0\xaf",2)),1007);
    Reject(Masked(std::string(65537,'x')),1009);
    auto bad = Masked("x"); bad[0] |= 0x40; Reject(bad);
    {
        Decoder d; Message m; std::string b = Masked(std::string(65536,'x'),1,false);
        Check(d.Next(b,m) == Result::Fragment,"fragment boundary"); b = Masked("x",0);
        Check(d.Next(b,m) == Result::Error && d.closeCode == 1009,"aggregate message limit");
    }
    using json = nlohmann::json;
    Check(ValidateCommand({{"action","playlists/tracks/get"},{"playlist_id",0},{"limit",1000},{"offset",0}}).empty(), "Companion track request");
    Check(!ValidateCommand(json::array()).empty(), "object required");
    Check(!ValidateCommand({{"action",4}}).empty(), "string action required");
    for (json v : {json(-1),json(1.5),json("1"),json(18446744073709551615ull)})
        Check(!ValidateCommand({{"action","playlists/get/one"},{"playlist_id",v}}).empty(), "integer range validated");
    Check(!ValidateCommand({{"action","player/volume/set"},{"volume",101}}).empty(), "volume bounded");
    Check(!ValidateCommand({{"action","playlists/tracks/get"},{"limit",1001}}).empty(), "track page cap");
    // Deterministic malformed-input stress test under ASan/UBSan.
    std::mt19937 rng(20261018);
    for (int i = 0; i < 50000; ++i) {
        std::string bytes(rng()%512,0); for(char& c:bytes)c=char(rng());
        Decoder d; Message m;
        for(int n=0;n<4 && !bytes.empty();++n) {
            auto r=d.Next(bytes,m); if(r==Result::NeedMore || r==Result::Error)break;
        }
        ValidUtf8(bytes); ValidJsonDepth(bytes); CheckHandshake(bytes); ValidAllowList(bytes);
    }
    std::cout << checks << " assertions passed; 50,000 malformed-input cases completed\n";
    return 0;
} catch(const std::exception& e) { std::cerr << "FAILED: " << e.what() << '\n'; return 1; }
