#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <cstddef>

namespace protocol {
constexpr std::size_t MaxHeader = 8192;
constexpr std::size_t MaxMessage = 65536;
struct Handshake { int status = 400; std::string key; };
Handshake CheckHandshake(std::string_view header);
bool ValidUtf8(std::string_view text);
bool ValidJsonDepth(std::string_view text, unsigned maxDepth = 32);
bool ParseIPv4(std::string_view text, std::uint32_t& address);
bool ValidAllowList(std::string_view list);
bool Allowed(std::uint32_t address, std::string_view list);
std::string Base64(const unsigned char* data, std::size_t size);
std::string Frame(std::string_view payload, unsigned opcode = 1);
struct Message { unsigned opcode = 0; std::string payload; };
enum class Result { NeedMore, Message, Fragment, Error };
class Decoder {
    std::string fragments_;
    bool fragmented_ = false;
public:
    unsigned closeCode = 1002;
    bool fragmented() const { return fragmented_; }
    Result Next(std::string& buffer, Message& message);
};
}
