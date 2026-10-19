#pragma once
#include "pch.h"

// All entry points must run on the AIMP main thread. The command dispatcher
// already serializes commands there; relative edits therefore read/set/read
// without stale client-side state or replaying disconnected encoder events.
json GetAudioCapabilities();
json GetAudioState();
bool HandleAudioCommand(const std::string& action, const json& command, json& response);
