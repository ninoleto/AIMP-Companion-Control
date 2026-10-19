#pragma once
#include "pch.h"

// Invoke only from AIMP's main thread, like the existing player API.
json GetLibraryCapabilities();
bool HandleLibraryCommand(const std::string& action, const json& cmd, json& response);

json GetTagCapabilities();
bool HandleTagCommand(const std::string& action, const json& cmd, json& response);
