#pragma once
#include "pch.h"
extern HINSTANCE g_hInstance;
extern IAIMPCore* g_core;
extern DWORD g_mainThreadId;
extern int g_port;
extern int g_bindMode; // 0: loopback; 1: all; 2: selected IPv4 address
extern std::wstring g_bindIp;
extern bool g_running;
extern bool g_allowListEnabled;
extern std::string g_allowList;
extern std::mutex g_focusMutex;
extern int g_focusPlaylistIdx;
extern int g_focusTrackIdx;
