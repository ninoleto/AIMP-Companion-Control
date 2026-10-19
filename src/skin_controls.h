#pragma once
#include "pch.h"

json GetSkinCapabilities();
json GetSkinState();
bool HandleSkinCommand(const std::string& action, const json& command, json& response);
