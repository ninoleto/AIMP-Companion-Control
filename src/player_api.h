#pragma once
#include "pch.h"

// Message API helpers
bool MsgGetBool(int propertyId);
bool MsgSetBool(int propertyId, bool value);
bool MsgToggleBool(int propertyId);

// Playlist/track JSON helpers (вызывать из главного потока)
std::string GetPlaylistName(IAIMPPlaylist* pl);
std::string GetPlaylistId(IAIMPPlaylist* pl);
int         GetPlayingIndex(IAIMPPlaylist* pl);
double      GetPlaylistDuration(IAIMPPlaylist* pl);
void        GetFileInfo(IAIMPPlaylistItem* item, json& out);

bool FillPlaylistJson(IAIMPServicePlaylistManager* mgr, int idx, json& out);
bool FillTrackJson(IAIMPServicePlaylistManager* mgr, int plIdx, int trackIdx, json& out);

// Воспроизвести: resume если paused, иначе найти оранжевый трек и Play2
// Используется в /api/player/play и WS action player/play
HRESULT PlayerPlay();

// REST API responses
json GetPlayerStatus();
json FocusPlaylistShift(int delta);
json FocusTrackShift(int delta);
json GetPlaylistsResponse();
json GetPlaylistResponse(int idx);
json GetTracksResponse(int plIdx, int limit, int offset);
json GetTrackResponse(int plIdx, int trackIdx);

struct ParsedPath {
    int playlistId = -1;
    int trackId    = -1;
    std::string action;
};

void DoPlaylistAction(const ParsedPath& pp, const std::string& method,
                      const std::string& body, json& rsp, int& code);
