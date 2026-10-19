#include "media_helpers.h"
#include <cassert>
#include <iostream>
#include <limits>

int main() {
    using namespace media_control;
    assert(Utf8Prefix("abcd", 2) == "ab");
    assert(Utf8Prefix("čisto", 1).empty());
    assert(Utf8Prefix("čisto", 2) == "č");
    assert(Utf8Prefix("A🎵B", 4) == "A");
    assert(Utf8Prefix("A🎵B", 5) == "A🎵");
    assert(Utf8Prefix("test", 0).empty());
    assert(ImageSizeAllowed(4096, 4096));
    assert(!ImageSizeAllowed(0, 144));
    assert(!ImageSizeAllowed(-1, 144));
    assert(!ImageSizeAllowed(4097, 144));
    assert(ThumbnailSize(3000, 2000) == std::make_pair(144, 96));
    assert(ThumbnailSize(2000, 3000) == std::make_pair(96, 144));
    assert(ThumbnailSize(1, 4096) == std::make_pair(1, 144));
    assert(ThumbnailSize(72, 72) == std::make_pair(72, 72));
    assert(ThumbnailSize(5000, 1) == std::make_pair(0, 0));
    int time = 0;
    assert(PlaybackMilliseconds(1.2345, time) && time == 1234);
    assert(PlaybackMilliseconds(0, time) && time == 0);
    assert(!PlaybackMilliseconds(-1, time));
    assert(!PlaybackMilliseconds(std::numeric_limits<double>::infinity(), time));
    assert(!PlaybackMilliseconds(std::numeric_limits<double>::quiet_NaN(), time));
    assert(!PlaybackMilliseconds(2147483.648, time));
    std::cout << "Media UTF-8, image limits/aspect ratio and lyrics time validation passed\n";
}
