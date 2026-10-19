#pragma once
#include "pch.h"
json GetMusicLibraryCapabilities();
bool HandleMusicLibraryCommand(const std::string& action, const json& cmd, json& response);
void FinalizeMusicLibraryControls();
