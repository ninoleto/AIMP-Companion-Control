#pragma once
#include "pch.h"
bool StartServer();
void StopServer();
void RestartServer();
std::wstring ServerStatusText();
// Callbacks only set atomic bits; SDK calls and socket IO run on the main thread.
void WsNotify(unsigned flags);
constexpr unsigned NotifyState = 1;
constexpr unsigned NotifyTrack = 2;
constexpr unsigned NotifyEnd = 4;
constexpr unsigned NotifyProperty = 8;
constexpr unsigned NotifyFocus = 16;
constexpr unsigned NotifyPosition = 32;
