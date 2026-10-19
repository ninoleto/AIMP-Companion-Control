#!/usr/bin/env python3
"""Run the actual playlist/track target resolvers against portable SDK doubles.

This checks target identity and failure behavior, including stale cached focus.
It does not validate AIMP's Windows COM implementation or calling conventions.
"""
from pathlib import Path
import os
import subprocess
import tempfile


root = Path(__file__).resolve().parents[1]
source = (root / "src/library_controls.cpp").read_text()
body = source[source.index("bool ResolvePlaylist("):source.index("\nvoid ReadInfo(")]

prefix = r'''
#include "json.hpp"
#include <algorithm>
#include <array>
#include <cassert>
#include <iostream>
#include <limits>
#include <mutex>
#include <string>
#include <vector>

using json = nlohmann::json;
using HRESULT = int;
constexpr HRESULT S_OK = 0, E_FAIL = -1, E_INVALIDARG = -2;
constexpr int IID_IAIMPPropertyList = 1, IID_IAIMPPlaylist = 2;
constexpr int IID_IAIMPPlaylistItem = 3, IID_IAIMPServicePlayer = 4;
constexpr int AIMP_PLAYLIST_PROPID_FOCUSINDEX = 5;
constexpr int AIMP_PLAYLISTITEM_PROPID_PLAYLIST = 6, AIMP_PLAYLISTITEM_PROPID_INDEX = 7;
bool SUCCEEDED(HRESULT hr) { return hr >= 0; }
bool FAILED(HRESULT hr) { return hr < 0; }

struct IUnknown {
    int refs = 1;
    void AddRef() { ++refs; }
    void Release() { assert(refs > 1); --refs; }
    virtual HRESULT QueryInterface(int, void**) { return E_FAIL; }
    virtual ~IUnknown() = default;
};
struct IAIMPPropertyList : IUnknown {
    int focus = -1, reads = 0;
    bool readFails = false;
    HRESULT readResult = S_OK;
    HRESULT GetValueAsInt32(int property, int* value) {
        assert(property == AIMP_PLAYLIST_PROPID_FOCUSINDEX); ++reads;
        if (readFails) return E_FAIL;
        *value = focus; return readResult;
    }
};
struct IAIMPPlaylist;
struct IAIMPPlaylistItem : IUnknown {
    IAIMPPlaylist* playlist = nullptr;
    int index = -1;
    HRESULT GetValueAsObject(int property, int iid, void** value);
    HRESULT GetValueAsInt32(int property, int* value) {
        assert(property == AIMP_PLAYLISTITEM_PROPID_INDEX);
        *value = index; return S_OK;
    }
};
struct IAIMPPlaylist : IUnknown {
    IAIMPPropertyList props;
    std::array<IAIMPPlaylistItem, 3> items;
    std::vector<int> itemReads;
    bool propertiesFail = false, itemReadFails = false;
    IAIMPPlaylist() {
        for (int i = 0; i < static_cast<int>(items.size()); ++i) {
            items[i].playlist = this; items[i].index = i;
        }
    }
    HRESULT QueryInterface(int iid, void** value) override {
        assert(iid == IID_IAIMPPropertyList);
        if (propertiesFail) return E_FAIL;
        props.AddRef(); *value = &props; return S_OK;
    }
    int GetItemCount() { return static_cast<int>(items.size()); }
    HRESULT GetItem(int index, int iid, void** value) {
        assert(iid == IID_IAIMPPlaylistItem); itemReads.push_back(index);
        assert(index >= 0 && index < GetItemCount());
        if (itemReadFails) return E_FAIL;
        items[index].AddRef(); *value = &items[index]; return S_OK;
    }
};
HRESULT IAIMPPlaylistItem::GetValueAsObject(int property, int iid, void** value) {
    assert(property == AIMP_PLAYLISTITEM_PROPID_PLAYLIST && iid == IID_IAIMPPlaylist);
    if (!playlist) return E_FAIL;
    playlist->AddRef(); *value = playlist; return S_OK;
}
struct IAIMPServicePlayer : IUnknown {
    IAIMPPlaylistItem* playing = nullptr;
    HRESULT GetPlaylistItem(IAIMPPlaylistItem** value) {
        if (!playing) return E_FAIL;
        playing->AddRef(); *value = playing; return S_OK;
    }
};
struct IAIMPServicePlaylistManager {
    std::vector<IAIMPPlaylist*> loaded;
    std::vector<int> loadedReads;
    IAIMPPlaylist* active = nullptr;
    IAIMPPlaylist* playing = nullptr;
    int activeReads = 0, playingReads = 0;
    bool activeFails = false;
    HRESULT activeResult = S_OK;
    int GetLoadedPlaylistCount() { return static_cast<int>(loaded.size()); }
    HRESULT GetLoadedPlaylist(int index, IAIMPPlaylist** value) {
        loadedReads.push_back(index);
        assert(index >= 0 && index < GetLoadedPlaylistCount());
        loaded[index]->AddRef(); *value = loaded[index]; return S_OK;
    }
    HRESULT GetActivePlaylist(IAIMPPlaylist** value) {
        ++activeReads;
        if (activeFails) return E_FAIL;
        *value = active;
        if (active) active->AddRef();
        return activeResult;
    }
    HRESULT GetPlayingPlaylist(IAIMPPlaylist** value) {
        ++playingReads; *value = playing;
        if (playing) playing->AddRef();
        return S_OK;
    }
};
template<class T> struct Ref {
    T* p = nullptr;
    Ref() = default;
    Ref(const Ref&) = delete;
    ~Ref() { if (p) p->Release(); }
    T* operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
    T** put() { return &p; }
    void** out() { return reinterpret_cast<void**>(&p); }
};
IAIMPServicePlayer* currentPlayer = nullptr;
bool Service(int iid, Ref<IAIMPServicePlayer>& value) {
    assert(iid == IID_IAIMPServicePlayer);
    if (!currentPlayer) return false;
    currentPlayer->AddRef(); *value.put() = currentPlayer; return true;
}
json Error(const char* code, const std::string& message = "") {
    json result = {{"error", {{"code", code}}}};
    if (!message.empty()) result["error"]["message"] = message;
    return result;
}
bool Check(HRESULT result, json& response, const char* operation) {
    if (SUCCEEDED(result)) return true;
    response = Error("AIMP_OPERATION_FAILED", operation);
    response["error"]["hresult"] = result;
    return false;
}
bool Integer(const json& cmd, const char* key, int& value, int minimum, int maximum, bool required = true) {
    if (!cmd.contains(key)) return !required;
    if (!cmd[key].is_number_integer()) return false;
    const double number = cmd[key].get<double>();
    if (number < minimum || number > maximum) return false;
    value = cmd[key].get<int>(); return true;
}
bool Properties(IUnknown* object, Ref<IAIMPPropertyList>& props, json& response) {
    if (!object || !Check(object->QueryInterface(IID_IAIMPPropertyList, props.out()), response, "Read object properties") || !props) {
        if (!response.contains("error")) response = Error("PROPERTIES_UNAVAILABLE");
        return false;
    }
    return true;
}
int PlaylistIndex(IAIMPServicePlaylistManager* manager, IAIMPPlaylist* playlist) {
    const auto found = std::find(manager->loaded.begin(), manager->loaded.end(), playlist);
    return found == manager->loaded.end() ? -1 : static_cast<int>(found - manager->loaded.begin());
}
// Deliberately stale values: old resolver code compiles but must fail the tests.
std::mutex g_focusMutex;
int g_focusPlaylistIdx = 0, g_focusTrackIdx = 0;

struct Fixture {
    IAIMPPlaylist first, second;
    IAIMPServicePlaylistManager manager;
    IAIMPServicePlayer player;
    Fixture() {
        manager.loaded = {&first, &second}; manager.active = &first;
        manager.playing = &second; player.playing = &second.items[1];
        currentPlayer = &player; g_focusPlaylistIdx = g_focusTrackIdx = 0;
    }
    ~Fixture() {
        assert(first.refs == 1 && second.refs == 1 && player.refs == 1);
        assert(first.props.refs == 1 && second.props.refs == 1);
        for (const auto* playlist : {&first, &second})
            for (const auto& item : playlist->items) assert(item.refs == 1);
        currentPlayer = nullptr;
    }
    int ItemReadCount() const { return first.itemReads.size() + second.itemReads.size(); }
};
'''

suffix = r'''
void ExpectTrack(Fixture& f, const json& cmd, IAIMPPlaylist* expected, int expectedPlaylist, int expectedTrack) {
    Ref<IAIMPPlaylist> playlist; Ref<IAIMPPlaylistItem> item;
    int playlistIndex = -1, trackIndex = -1; json response;
    assert(ResolveTrack(&f.manager, cmd, playlist, item, playlistIndex, trackIndex, response));
    assert(!response.contains("error") && playlist.p == expected);
    assert(item.p == &expected->items[expectedTrack]);
    assert(playlistIndex == expectedPlaylist && trackIndex == expectedTrack);
}
void ExpectTrackError(Fixture& f, const json& cmd, const char* code) {
    Ref<IAIMPPlaylist> playlist; Ref<IAIMPPlaylistItem> item;
    int playlistIndex = -1, trackIndex = -1; json response;
    assert(!ResolveTrack(&f.manager, cmd, playlist, item, playlistIndex, trackIndex, response));
    assert(response.contains("error") && response["error"]["code"] == code && !item);
}
void ExpectPlaylist(Fixture& f, const json& cmd, IAIMPPlaylist* expected, int expectedIndex) {
    Ref<IAIMPPlaylist> playlist; int index = -1; json response;
    assert(ResolvePlaylist(&f.manager, cmd, playlist, index, response));
    assert(!response.contains("error") && playlist.p == expected && index == expectedIndex);
}
int main() {
    int scenarios = 0;
    { // No SDK focus must not silently pick cached track zero.
        Fixture f; f.first.props.focus = -1;
        ExpectTrackError(f, {}, "NO_FOCUSED_TRACK"); assert(f.ItemReadCount() == 0); ++scenarios;
    }
    {
        Fixture f; f.first.props.readFails = true;
        ExpectTrackError(f, {}, "NO_FOCUSED_TRACK"); assert(f.ItemReadCount() == 0); ++scenarios;
    }
    {
        Fixture f; f.first.propertiesFail = true;
        ExpectTrackError(f, {}, "NO_FOCUSED_TRACK"); assert(f.ItemReadCount() == 0); ++scenarios;
    }
    {
        Fixture f; f.first.props.focus = f.first.GetItemCount();
        ExpectTrackError(f, {}, "NO_FOCUSED_TRACK"); assert(f.ItemReadCount() == 0); ++scenarios;
    }
    { // S_FALSE must not authorize a focus read, even if an index was provided.
        Fixture f; f.first.props.focus = 0; f.first.props.readResult = 1;
        ExpectTrackError(f, {}, "NO_FOCUSED_TRACK"); assert(f.ItemReadCount() == 0); ++scenarios;
    }
    { // Loaded tab zero exists, but no tab is active. Never fall back to tab zero.
        Fixture f; f.manager.active = nullptr; f.first.props.focus = 0;
        ExpectTrackError(f, {}, "NO_FOCUSED_PLAYLIST");
        assert(f.manager.loadedReads.empty() && f.ItemReadCount() == 0); ++scenarios;
    }
    {
        Fixture f; f.manager.activeFails = true; f.first.props.focus = 0;
        ExpectTrackError(f, {}, "NO_FOCUSED_PLAYLIST");
        assert(f.manager.loadedReads.empty() && f.ItemReadCount() == 0); ++scenarios;
    }
    {
        Fixture f; f.manager.activeResult = 1; f.first.props.focus = 0;
        ExpectTrackError(f, {}, "NO_FOCUSED_PLAYLIST");
        assert(f.manager.loadedReads.empty() && f.ItemReadCount() == 0); ++scenarios;
    }
    { // An active-object reply that is no longer loaded is not a valid target.
        Fixture f; f.manager.loaded = {&f.second}; f.first.props.focus = 0;
        ExpectTrackError(f, {}, "NO_FOCUSED_PLAYLIST");
        assert(f.ItemReadCount() == 0); ++scenarios;
    }
    { // A real SDK focus index zero is valid, even when caches disagree.
        Fixture f; f.first.props.focus = 0; g_focusPlaylistIdx = 1; g_focusTrackIdx = 2;
        ExpectTrack(f, {}, &f.first, 0, 0);
        assert(f.manager.activeReads == 1 && f.first.props.reads == 1); ++scenarios;
    }
    { // Each command observes the current active tab and its current focus.
        Fixture f; f.manager.active = &f.second; f.second.props.focus = 1;
        ExpectTrack(f, {{"target", "focused"}}, &f.second, 1, 1);
        f.manager.active = &f.first; f.first.props.focus = 2;
        ExpectTrack(f, {{"target", "focused"}}, &f.first, 0, 2);
        assert(f.manager.activeReads == 2); ++scenarios;
    }
    {
        Fixture f; f.manager.loaded = {&f.second, &f.first}; f.first.props.focus = 1;
        ExpectTrack(f, {}, &f.first, 1, 1); ++scenarios;
    }
    { // Explicit targets must ignore focus and active-tab state entirely.
        Fixture f; f.manager.active = &f.second;
        ExpectTrack(f, {{"playlist_id", 0}, {"track_id", 2}}, &f.first, 0, 2);
        assert(f.manager.activeReads == 0 && f.first.props.reads == 0); ++scenarios;
    }
    {
        Fixture f; ExpectTrackError(f, {{"playlist_id", 0}, {"track_id", -1}}, "INVALID_TRACK_ID");
        assert(f.ItemReadCount() == 0); ++scenarios;
    }
    {
        Fixture f; ExpectTrackError(f, {{"playlist_id", 0}, {"track_id", 3}}, "TRACK_NOT_FOUND");
        assert(f.ItemReadCount() == 0); ++scenarios;
    }
    { // Playing targets remain resolved by the player service, not UI focus.
        Fixture f; ExpectTrack(f, {{"target", "playing"}}, &f.second, 1, 1);
        assert(f.manager.activeReads == 0 && f.ItemReadCount() == 0); ++scenarios;
    }
    {
        Fixture f; f.player.playing = nullptr;
        ExpectTrackError(f, {{"target", "playing"}}, "NO_PLAYING_TRACK");
        assert(f.ItemReadCount() == 0); ++scenarios;
    }
    {
        Fixture f; f.manager.active = &f.second;
        ExpectPlaylist(f, {{"target", "focused"}}, &f.second, 1);
        assert(f.manager.activeReads == 1); ++scenarios;
    }
    {
        Fixture f; ExpectPlaylist(f, {{"target", "playing"}}, &f.second, 1);
        assert(f.manager.playingReads == 1 && f.manager.activeReads == 0); ++scenarios;
    }
    {
        Fixture f; ExpectPlaylist(f, {}, &f.first, 0);
        assert(f.manager.activeReads == 1); ++scenarios;
    }
    {
        Fixture f; f.first.props.focus = 0; f.first.itemReadFails = true;
        ExpectTrackError(f, {}, "AIMP_OPERATION_FAILED");
        assert(f.ItemReadCount() == 1); ++scenarios;
    }
    std::cout << "Focus target resolution: " << scenarios << " scenarios passed\n";
}
'''

with tempfile.TemporaryDirectory(prefix="aimp-focus-test-") as folder:
    harness = Path(folder) / "focus.cpp"
    executable = Path(folder) / "focus-tests"
    harness.write_text(prefix + body + suffix)
    subprocess.run([
        os.environ.get("CXX", "g++"), "-std=c++17", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
        "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
        "-isystem", str(root / "third_party"), str(harness), "-o", str(executable),
    ], check=True)
    # LeakSanitizer cannot inspect /proc in this executor; ASan/UBSan remain on.
    subprocess.run([str(executable)], check=True, env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0"})
