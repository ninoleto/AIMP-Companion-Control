#pragma once
#include "pch.h"

// Main-thread-only SDK controls. Failed reads remain null, never fabricated values.
json GetPlayerExtensionCapabilities();
json GetPlayerExtensionState();
bool HandlePlayerExtensionCommand(const std::string& action, const json& cmd, json& rsp);
// Call before releasing g_core / unloading the plug-in; cancels queued UI commands.
void FinalizePlayerExtensions();
