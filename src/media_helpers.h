#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>

namespace media_control {
constexpr std::size_t MaxArtworkBytes = 262144;
constexpr int MaxArtworkDimension = 144;
constexpr std::size_t MaxSourceImageBytes = 4 * 1024 * 1024;
constexpr std::size_t MaxLyricsBytes = 65536;
constexpr std::size_t MaxLineBytes = 2048;
constexpr int MaxLyricsLines = 1000;

inline std::string Utf8Prefix(std::string value, std::size_t limit) {
    if (value.size() <= limit) return value;
    std::size_t end = limit;
    while (end && (static_cast<unsigned char>(value[end]) & 0xc0) == 0x80) --end;
    value.resize(end);
    return value;
}

inline bool ImageSizeAllowed(long width, long height) {
    return width > 0 && height > 0 && width <= 4096 && height <= 4096 &&
           static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height) <= 16777216;
}

inline std::pair<int, int> ThumbnailSize(long width, long height) {
    if (!ImageSizeAllowed(width, height)) return {0, 0};
    const double scale = std::min(1.0, static_cast<double>(MaxArtworkDimension) / std::max(width, height));
    return {std::max(1, static_cast<int>(std::floor(width * scale))),
            std::max(1, static_cast<int>(std::floor(height * scale)))};
}

inline bool PlaybackMilliseconds(double seconds, int& value) {
    if (!std::isfinite(seconds) || seconds < 0 || seconds > std::numeric_limits<int>::max() / 1000.0) return false;
    value = static_cast<int>(std::floor(seconds * 1000.0));
    return true;
}
} // namespace media_control
