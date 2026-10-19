#include "focus_sync.h"
#include "globals.h"
#include "ws_server.h"
#include "commands.h"

// All command entry points are dispatched by the main-thread socket pump.
// Focus access is synchronous on that same thread.
bool RunInMainThread(std::function<void()> fn) {
    if (!g_core || GetCurrentThreadId() != g_mainThreadId) return false;
    fn();
    return true;
}

int GetFocusedIndex(IAIMPPlaylist* pl) {
    int idx = -1;
    PlaylistProps p(pl);
    if (p) p->GetValueAsInt32(AIMP_PLAYLIST_PROPID_FOCUSINDEX, &idx);
    return idx;
}

void SetPlaylistFocusAndSelection(IAIMPPlaylist* pl, int trackIdx) {
    if (!pl) return;
    PlaylistProps props(pl);
    if (props) props->SetValueAsInt32(AIMP_PLAYLIST_PROPID_FOCUSINDEX, trackIdx);
    int count = pl->GetItemCount();
    for (int i = 0; i < count; i++) {
        IAIMPPlaylistItem* item = nullptr;
        if (pl->GetItem(i, IID_IAIMPPlaylistItem, (void**)&item) == S_OK && item) {
            item->SetValueAsInt32(AIMP_PLAYLISTITEM_PROPID_SELECTED, (i == trackIdx) ? 1 : 0);
            item->Release();
        }
    }
}

int FindPlaylistIndex(IAIMPPlaylist* targetPl) {
    if (!g_core || !targetPl) return -1;
    IAIMPServicePlaylistManager* mgr = nullptr;
    if (g_core->QueryInterface(IID_IAIMPServicePlaylistManager, (void**)&mgr) != S_OK || !mgr)
        return -1;
    int count = mgr->GetLoadedPlaylistCount();
    int found = -1;
    for (int i = 0; i < count; i++) {
        IAIMPPlaylist* pl = nullptr;
        if (mgr->GetLoadedPlaylist(i, &pl) == S_OK && pl) {
            if (pl == targetPl) { found = i; pl->Release(); break; }
            pl->Release();
        }
    }
    mgr->Release();
    return found;
}


class MessageHook final : public IAIMPMessageHook {
    LONG refs_ = 1;
public:
    HRESULT WINAPI QueryInterface(REFIID id, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (id != IID_IUnknown && id != IID_IAIMPMessageHook) return E_NOINTERFACE;
        *out = static_cast<IAIMPMessageHook*>(this); AddRef(); return S_OK;
    }
    ULONG WINAPI AddRef() override { return InterlockedIncrement(&refs_); }
    ULONG WINAPI Release() override {
        LONG n = InterlockedDecrement(&refs_); if (!n) delete this; return n;
    }
    void WINAPI CoreMessage(LongWord message, int, void*, HRESULT*) override {
        switch (message) {
        case AIMP_MSG_EVENT_PLAYER_STATE: WsNotify(NotifyState); break;
        case AIMP_MSG_EVENT_STREAM_START:
        case AIMP_MSG_EVENT_STREAM_START_SUBTRACK:
        case AIMP_MSG_EVENT_PLAYING_FILE_INFO: WsNotify(NotifyTrack); break;
        case AIMP_MSG_EVENT_STREAM_END: WsNotify(NotifyEnd); break;
        case AIMP_MSG_EVENT_PROPERTY_VALUE: WsNotify(NotifyProperty); break;
        case AIMP_MSG_EVENT_PLAYER_UPDATE_POSITION:
        case AIMP_MSG_EVENT_PLAYER_UPDATE_POSITION_HR: WsNotify(NotifyPosition); break;
        }
    }
};
static MessageHook* hook = nullptr;
void RefreshFocusFromPlayer() {
    if (!g_core || GetCurrentThreadId() != g_mainThreadId) return;
    IAIMPServicePlaylistManager* mgr = nullptr;
    if (FAILED(g_core->QueryInterface(IID_IAIMPServicePlaylistManager, (void**)&mgr)) || !mgr) return;
    int playlistIndex = 0, trackIndex = 0;
    IAIMPPlaylist* active = nullptr;
    if (SUCCEEDED(mgr->GetActivePlaylist(&active)) && active) {
        playlistIndex = std::max(0, FindPlaylistIndex(active));
        trackIndex = std::max(0, GetFocusedIndex(active));
        active->Release();
    }
    mgr->Release();
    bool changed;
    {
        std::lock_guard<std::mutex> lock(g_focusMutex);
        changed = playlistIndex != g_focusPlaylistIdx || trackIndex != g_focusTrackIdx;
        g_focusPlaylistIdx = playlistIndex; g_focusTrackIdx = trackIndex;
    }
    if (changed) WsNotify(NotifyFocus);
}
void InitFocusSync() {
    IAIMPServiceMessageDispatcher* dispatcher = nullptr;
    if (g_core && SUCCEEDED(g_core->QueryInterface(IID_IAIMPServiceMessageDispatcher, (void**)&dispatcher)) && dispatcher) {
        hook = new MessageHook();
        if (FAILED(dispatcher->Hook(hook))) { hook->Release(); hook = nullptr; }
        dispatcher->Release();
    }
    if (IsAimpLoaded()) RefreshFocusFromPlayer();
}
void FinalizeFocusSync() {
    IAIMPServiceMessageDispatcher* dispatcher = nullptr;
    if (hook && g_core && SUCCEEDED(g_core->QueryInterface(IID_IAIMPServiceMessageDispatcher, (void**)&dispatcher)) && dispatcher) {
        dispatcher->Unhook(hook); dispatcher->Release();
    }
    if (hook) { hook->Release(); hook = nullptr; }
}
