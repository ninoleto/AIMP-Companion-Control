#pragma once
#include "pch.h"
json GetAppControlCapabilities();
json GetAppControlState();
bool HandleAppControlCommand(const std::string& action, const json& cmd, json& response);
void FinalizeAppControls();
