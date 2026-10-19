#pragma once
#include "pch.h"

// All public entry points and documented AIMP callbacks run on the main thread.
// Initialize after g_core/g_mainThreadId; finalize before releasing g_core.
void InitializeMediaControls();
void FinalizeMediaControls();
json GetMediaCapabilities();
json GetMediaState();
bool HandleMediaCommand(const std::string& action, const json& command, json& response);
