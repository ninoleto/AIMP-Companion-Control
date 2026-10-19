#include "globals.h"
HINSTANCE g_hInstance = nullptr;
IAIMPCore* g_core = nullptr;
DWORD g_mainThreadId = 0;
int g_port = 19122;
int g_bindMode = 0;
std::wstring g_bindIp;
bool g_running = false;
bool g_allowListEnabled = true;
std::string g_allowList = "127.0.0.1";
std::mutex g_focusMutex;
int g_focusPlaylistIdx = 0;
int g_focusTrackIdx = 0;
