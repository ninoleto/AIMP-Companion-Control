#include "commands.h"
#include "globals.h"
#include "player_api.h"
#include "focus_sync.h"
#include "command_validation.h"
#include "audio_controls.h"
#include "player_extensions.h"
#include "library_controls.h"
#include "media_controls.h"
#include "music_library_controls.h"
#include "app_controls.h"
#include "skin_controls.h"

bool IsAimpLoaded() {
    if (!g_core) return false;
    IAIMPServiceMessageDispatcher* dispatcher = nullptr;
    if (g_core->QueryInterface(IID_IAIMPServiceMessageDispatcher, reinterpret_cast<void**>(&dispatcher)) != S_OK || !dispatcher)
        return false;
    BOOL loaded = FALSE;
    HRESULT hr = dispatcher->Send(AIMP_MSG_PROPERTY_LOADED, AIMP_MSG_PROPVALUE_GET, &loaded);
    dispatcher->Release();
    return hr == S_OK && loaded != FALSE;
}

template<class F> static json WithPlayer(F fn) {
    IAIMPServicePlayer* player = nullptr;
    if (!g_core || FAILED(g_core->QueryInterface(IID_IAIMPServicePlayer, (void**)&player)) || !player)
        return {{"error", {{"code", "PLAYER_UNAVAILABLE"}}}};
    struct Release { IAIMPServicePlayer* p; ~Release() { p->Release(); } } release{player};
    return fn(player);
}
static json SdkResult(HRESULT result, json success) {
    if (FAILED(result)) return {{"error", {{"code", "AIMP_OPERATION_FAILED"}, {"hresult", result}}}};
    return success;
}

json ExecuteCommand(const json& cmd) {
    std::string error = ValidateCommand(cmd);
    std::string action = cmd.is_object() && cmd.contains("action") && cmd["action"].is_string() ? cmd["action"].get<std::string>() : "";
    if (!error.empty()) return {{"action", action}, {"error", {{"code", error}}}};
    if (!IsAimpLoaded()) return {{"action", action}, {"error", {{"code", "AIMP_LOADING"}}}};
    json rsp;
    try {
        RefreshFocusFromPlayer();
if (action == "capabilities/get") {
    json capabilities = GetAudioCapabilities();
    capabilities.update(GetPlayerExtensionCapabilities());
    capabilities["library"] = GetLibraryCapabilities();
    capabilities["tags"] = GetTagCapabilities();
    capabilities.update(GetMediaCapabilities());
    capabilities["music_library"] = GetMusicLibraryCapabilities();
    capabilities.update(GetAppControlCapabilities());
    capabilities.update(GetSkinCapabilities());
    rsp = {{"protocol_version", 2}, {"plugin_version", "0.3.1"}, {"capabilities", capabilities}};
} else if (action == "extensions/get") {
    rsp = GetAudioState();
    rsp.update(GetPlayerExtensionState());
    rsp.update(GetMediaState());
    rsp.update(GetAppControlState());
    rsp.update(GetSkinState());
    json queue;
    if (HandleLibraryCommand("queue/get", {{"action", "queue/get"}, {"limit", 1}}, queue) && !queue.contains("error"))
        rsp.update(queue);
    else
        rsp["queue"] = {{"count", nullptr}, {"suspended", nullptr}, {"items", json::array()},
                        {"read_error", queue.value("error", json({{"code", "QUEUE_UNAVAILABLE"}}))}};
} else if (HandleAudioCommand(action, cmd, rsp) || HandlePlayerExtensionCommand(action, cmd, rsp) ||
           HandleLibraryCommand(action, cmd, rsp) || HandleTagCommand(action, cmd, rsp) ||
           HandleMediaCommand(action, cmd, rsp) || HandleMusicLibraryCommand(action, cmd, rsp) ||
           HandleAppControlCommand(action, cmd, rsp) || HandleSkinCommand(action, cmd, rsp)) {
    // New commands are isolated from the original bridge command paths.
} else if (action == "player/status") {
    rsp = GetPlayerStatus();
    rsp["event"] = "status";

} else if (action == "player/play") {
    rsp = SdkResult(PlayerPlay(), {{"ok", true}});
} else if (action == "player/pause") {
    rsp = WithPlayer([](auto* p) { return SdkResult(p->Pause(), {{"state", "paused"}}); });
} else if (action == "player/stop") {
    rsp = WithPlayer([](auto* p) { return SdkResult(p->Stop(), {{"state", "stopped"}}); });
} else if (action == "player/next") {
    rsp = WithPlayer([](auto* p) { return SdkResult(p->GoToNext(), {{"ok", true}}); });
} else if (action == "player/prev") {
    rsp = WithPlayer([](auto* p) { return SdkResult(p->GoToPrev(), {{"ok", true}}); });
} else if (action == "player/volume/get") {
    rsp = WithPlayer([](auto* p) {
        float volume = 0; BOOL muted = FALSE;
        HRESULT hr = p->GetVolume(&volume);
        if (SUCCEEDED(hr)) hr = p->GetMute(&muted);
        return SdkResult(hr, {{"volume", int(std::lround(volume * 100))}, {"muted", muted != FALSE}});
    });
} else if (action == "player/volume/set") {
    if (!cmd.contains("volume")) rsp = {{"error", {{"code", "INVALID_VOLUME"}}}};
    else rsp = WithPlayer([&](auto* p) {
        float volume = cmd["volume"].get<float>() / 100.0f;
        return SdkResult(p->SetVolume(volume), {{"volume", int(std::lround(volume * 100))}});
    });
} else if (action == "player/mute") {
    rsp = WithPlayer([](auto* p) {
        BOOL muted = FALSE; HRESULT hr = p->GetMute(&muted);
        if (SUCCEEDED(hr)) hr = p->SetMute(!muted);
        return SdkResult(hr, {{"muted", muted == FALSE}});
    });
} else if (action == "player/position/set") {
    if (!cmd.contains("position")) rsp = {{"error", {{"code", "INVALID_POSITION"}}}};
    else rsp = WithPlayer([&](auto* p) {
        double position = cmd["position"].get<double>();
        return SdkResult(p->SetPosition(position), {{"position", position}});
    });

} else if (action == "player/shuffle/get") {
    rsp["shuffle"] = MsgGetBool(AIMP_MSG_PROPERTY_SHUFFLE);

} else if (action == "player/shuffle/toggle") {
    rsp["shuffle"] = MsgToggleBool(AIMP_MSG_PROPERTY_SHUFFLE);

} else if (action == "player/repeat/get") {
    rsp["repeat"] = MsgGetBool(AIMP_MSG_PROPERTY_REPEAT);

} else if (action == "player/repeat/toggle") {
    rsp["repeat"] = MsgToggleBool(AIMP_MSG_PROPERTY_REPEAT);

} else if (action == "player/auto-jump/get") {
    rsp["auto_jump"] = MsgGetBool(AIMP_MSG_PROPERTY_AUTOJUMP_TO_NEXT_TRACK);

} else if (action == "player/auto-jump/toggle") {
    rsp["auto_jump"] = MsgToggleBool(AIMP_MSG_PROPERTY_AUTOJUMP_TO_NEXT_TRACK);

} else if (action == "focus/get") {
    int plIdx, trIdx;
    { std::lock_guard<std::mutex> lk(g_focusMutex); plIdx = g_focusPlaylistIdx; trIdx = g_focusTrackIdx; }
    RunInMainThread([&]() {
        IAIMPServicePlaylistManager* mgr = nullptr;
        if (g_core->QueryInterface(IID_IAIMPServicePlaylistManager, (void**)&mgr) != S_OK || !mgr) {
            rsp["error"] = "playlist manager unavailable"; return;
        }
        json fp; FillPlaylistJson(mgr, plIdx, fp); rsp["focus_playlist"] = fp;
        json ft; FillTrackJson(mgr, plIdx, trIdx, ft); rsp["focus_track"] = ft;
        mgr->Release();
    });

} else if (action == "focus/playlist/next") {
    rsp = FocusPlaylistShift(+1);

} else if (action == "focus/playlist/prev") {
    rsp = FocusPlaylistShift(-1);

} else if (action == "focus/track/next") {
    rsp = FocusTrackShift(+1);

} else if (action == "focus/track/prev") {
    rsp = FocusTrackShift(-1);

} else if (action == "focus/play") {
    int plIdx, trIdx;
    { std::lock_guard<std::mutex> lk(g_focusMutex); plIdx = g_focusPlaylistIdx; trIdx = g_focusTrackIdx; }
    int code = 200;
    ParsedPath pp; pp.playlistId = plIdx; pp.trackId = trIdx; pp.action = "play";
    DoPlaylistAction(pp, "POST", "", rsp, code);

} else if (action == "playlists/get") {
    rsp = GetPlaylistsResponse();

} else if (action == "playlists/get/one") {
    // {"action":"playlists/get/one","playlist_id":0}
    int plId = cmd.value("playlist_id", -1);
    if (plId >= 0) rsp = GetPlaylistResponse(plId);
    else rsp["error"]["code"] = "INVALID_PLAYLIST_ID";

} else if (action == "playlists/tracks/get") {
    // {"action":"playlists/tracks/get","playlist_id":0,"limit":50,"offset":0}
    int plId  = cmd.value("playlist_id", -1);
    int limit = cmd.value("limit", 50);
    int off   = cmd.value("offset", 0);
    if (plId >= 0) rsp = GetTracksResponse(plId, limit, off);
    else rsp["error"]["code"] = "INVALID_PLAYLIST_ID";

} else if (action == "playlists/track/get") {
    // {"action":"playlists/track/get","playlist_id":0,"track_id":5}
    int plId = cmd.value("playlist_id", -1);
    int trId = cmd.value("track_id", -1);
    if (plId >= 0 && trId >= 0) rsp = GetTrackResponse(plId, trId);
    else rsp["error"]["code"] = "INVALID_ID";

} else if (action == "playlists/play") {
    // {"action":"playlists/play","playlist_id":0}
    int plId = cmd.value("playlist_id", -1);
    int code = 200;
    ParsedPath pp; pp.playlistId = plId; pp.trackId = -1; pp.action = "play";
    if (plId >= 0) DoPlaylistAction(pp, "POST", "", rsp, code);
    else rsp["error"]["code"] = "INVALID_PLAYLIST_ID";

} else if (action == "playlists/resume") {
    // {"action":"playlists/resume","playlist_id":0}
    int plId = cmd.value("playlist_id", -1);
    int code = 200;
    ParsedPath pp; pp.playlistId = plId; pp.trackId = -1; pp.action = "resume";
    if (plId >= 0) DoPlaylistAction(pp, "POST", "", rsp, code);
    else rsp["error"]["code"] = "INVALID_PLAYLIST_ID";

} else if (action == "playlists/select") {
    // {"action":"playlists/select","playlist_id":0}
    int plId = cmd.value("playlist_id", -1);
    int code = 200;
    ParsedPath pp; pp.playlistId = plId; pp.trackId = -1; pp.action = "select";
    if (plId >= 0) DoPlaylistAction(pp, "POST", "", rsp, code);
    else rsp["error"]["code"] = "INVALID_PLAYLIST_ID";

} else if (action == "playlists/track/play") {
    // {"action":"playlists/track/play","playlist_id":0,"track_id":5}
    int plId = cmd.value("playlist_id", -1);
    int trId = cmd.value("track_id", -1);
    int code = 200;
    ParsedPath pp; pp.playlistId = plId; pp.trackId = trId; pp.action = "play";
    if (plId >= 0 && trId >= 0) DoPlaylistAction(pp, "POST", "", rsp, code);
    else rsp["error"]["code"] = "INVALID_ID";

} else if (action == "playlists/track/select") {
    // {"action":"playlists/track/select","playlist_id":0,"track_id":5}
    int plId = cmd.value("playlist_id", -1);
    int trId = cmd.value("track_id", -1);
    int code = 200;
    ParsedPath pp; pp.playlistId = plId; pp.trackId = trId; pp.action = "select";
    if (plId >= 0 && trId >= 0) DoPlaylistAction(pp, "POST", "", rsp, code);
    else rsp["error"]["code"] = "INVALID_ID";

} else {
    rsp["error"]["code"] = "UNKNOWN_ACTION";
    rsp["action"] = action;

}

    } catch (...) { rsp = {{"error", {{"code", "COMMAND_FAILED"}}}}; }
    if (rsp.is_null() || rsp.empty()) rsp = {{"error", {{"code", "PLAYER_UNAVAILABLE"}}}};
    rsp["action"] = action;
    return rsp;
}
