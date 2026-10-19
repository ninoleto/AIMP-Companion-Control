#pragma once

// Wire values deliberately use the documented SDK units, not UI percentages.
// Sources: AIMP 5.40 SDK apiMessages.h and its English reference help.
#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include "json.hpp"

namespace audio_control {
using Json = nlohmann::json;

struct Parameter {
    const char* id;
    const char* label;
    int messageOffset;
    double minimum;
    double maximum;
    double neutral;
    double step;
    const char* unit;
    bool boolean;
};

inline constexpr std::array<Parameter, 11> Parameters{{
    {"balance", "Balance", 3, -1, 1, 0, 0.01, "", false},
    {"chorus", "Chorus", 4, 0, 1, 0, 0.01, "", false},
    {"echo", "Echo", 5, 0, 1, 0, 0.01, "", false},
    // The SDK header says 1..4 while the CHM says 1..5. Use their common range.
    {"enhancer", "Enhancer", 6, 1, 4, 1, 0.05, "", false},
    {"flanger", "Flanger", 7, 0, 1, 0, 0.01, "", false},
    {"reverb", "Reverb", 8, 0, 1, 0, 0.01, "", false},
    {"pitch", "Pitch", 9, -10, 10, 0, 0.1, "", false},
    {"speed", "Speed", 10, 0.5, 1.5, 1, 0.01, "x", false},
    {"tempo", "Tempo", 11, 0.8, 1.5, 1, 0.01, "x", false},
    {"bass", "Bass enhancement", 12, 0, 2, 0, 0.05, "", false},
    {"equalizer_auto", "Automatic equalizer preset", 36, 0, 1, 0, 1, "", true},
}};

inline constexpr double EqMinimum = -15;
inline constexpr double EqMaximum = 15;
inline constexpr int MaxBands = 256;
inline constexpr int MaxPresets = 1024;

inline const Parameter* FindParameter(const std::string& id) {
    for (const auto& parameter : Parameters) if (id == parameter.id) return &parameter;
    return nullptr;
}

inline bool SafePresetName(const std::string& name) {
    if (name.empty() || name.size() > 512 || name.front() == ' ' || name.back() == ' ' || name.back() == '.') return false;
    for (unsigned char character : name)
        if (character < 32 || character == 127 || std::string("\\/:*?\"<>|").find(character) != std::string::npos) return false;
    return true;
}

inline bool FiniteNumber(const Json& object, const char* key, double& result) {
    if (!object.is_object() || !object.contains(key) || !object[key].is_number()) return false;
    result = object[key].get<double>();
    return std::isfinite(result);
}

inline double AdjustClamped(double current, double delta, double minimum, double maximum) {
    // Compare before adding, so even a finite DBL_MAX delta cannot overflow.
    if (delta >= maximum - current) return maximum;
    if (delta <= minimum - current) return minimum;
    return current + delta;
}

inline bool NumberTarget(const Json& command, bool relative, double current,
                         double minimum, double maximum, double& target) {
    double value = 0;
    if (!FiniteNumber(command, relative ? "delta" : "value", value)) return false;
    if (relative) {
        if (!std::isfinite(current)) return false;
        target = AdjustClamped(current, value, minimum, maximum);
        return true;
    }
    if (value < minimum || value > maximum) return false;
    target = value;
    return true;
}

inline bool BandIndex(const Json& command, int count, int& result) {
    if (count < 0 || count > MaxBands || !command.is_object() || !command.contains("band") ||
        !command["band"].is_number_integer()) return false;
    const auto& value = command["band"];
    if (value.is_number_unsigned()) {
        const auto index = value.get<std::uint64_t>();
        if (index >= static_cast<std::uint64_t>(count)) return false;
        result = static_cast<int>(index);
    } else {
        const auto index = value.get<std::int64_t>();
        if (index < 0 || index >= count) return false;
        result = static_cast<int>(index);
    }
    return true;
}

inline double DisplayNumber(double value) {
    // Single-precision SDK properties otherwise produce noisy JSON values.
    if (std::abs(value) > 1e12) return value;
    return std::round(value * 1000000.0) / 1000000.0;
}
} // namespace audio_control
