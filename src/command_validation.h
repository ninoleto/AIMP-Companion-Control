#pragma once
#include "json.hpp"
#include <cmath>
#include <limits>
#include <string>
inline std::string ValidateCommand(const nlohmann::json& cmd) {
    if (!cmd.is_object() || !cmd.contains("action") || !cmd["action"].is_string() ||
        cmd["action"].get_ref<const std::string&>().size() > 80) return "INVALID_COMMAND";
    for (const char* key : {"playlist_id", "track_id", "offset", "limit"}) {
        if (!cmd.contains(key)) continue;
        const auto& value = cmd[key];
        if (!value.is_number_integer() || value.get<double>() < 0 ||
            value.get<double>() > std::numeric_limits<int>::max()) return "INVALID_PARAMETER";
        if (std::string(key) == "limit" && (value.get<int>() < 1 || value.get<int>() > 1000)) return "INVALID_LIMIT";
    }
    for (const char* key : {"volume", "position"}) {
        if (!cmd.contains(key)) continue;
        if (!cmd[key].is_number()) return "INVALID_PARAMETER";
        double value = cmd[key].get<double>();
        if (!std::isfinite(value) || value < 0 || value > (std::string(key) == "volume" ? 100.0 : 1e9))
            return "INVALID_PARAMETER";
    }
    return {};
}
