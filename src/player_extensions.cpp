#include "player_extensions.h"
#include "globals.h"
#include "utils.h"
#include "ui_lifetime.h"
#include <cmath>
#include <limits>

namespace {
template<class T> struct Ref {
    T* p = nullptr;
    ~Ref() { if (p) p->Release(); }
    Ref() = default;
    Ref(const Ref&) = delete;
    Ref& operator=(const Ref&) = delete;
    T* operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
};

json Failure(const char* code, HRESULT hr = E_FAIL) {
    return {{"error", {{"code", code}, {"hresult", static_cast<long>(hr)}}}};
}
HRESULT Dispatcher(Ref<IAIMPServiceMessageDispatcher>& d) {
    return g_core ? g_core->QueryInterface(IID_IAIMPServiceMessageDispatcher, reinterpret_cast<void**>(&d.p)) : E_UNEXPECTED;
}
HRESULT PlayerProperties(Ref<IAIMPPropertyList>& p) {
    Ref<IAIMPServicePlayer> player;
    HRESULT hr = g_core ? g_core->QueryInterface(IID_IAIMPServicePlayer, reinterpret_cast<void**>(&player.p)) : E_UNEXPECTED;
    return hr == S_OK && player ? player->QueryInterface(IID_IAIMPPropertyList, reinterpret_cast<void**>(&p.p)) : hr;
}

enum class Kind { Boolean, Integer, Enum };
struct Option {
    const char* key;
    const char* label;
    int id;
    Kind kind;
    bool message;
    int maximum;
};
// Source: official SDK 5.40 apiMessages.h and IAIMPServicePlayer.html.
// Timing properties are Int32 milliseconds; the SDK documents 0 = off but
// does not document a smaller maximum. AIMP may reject/clamp a supplied value;
// every successful write is followed by an actual read.
const Option playerOptions[] = {
    {"action_on_end_of_track", "After current track", AIMP_MSG_PROPERTY_ACTION_ON_END_OF_TRACK, Kind::Enum, true, 2},
    {"action_on_end_of_playlist", "After current playlist", AIMP_MSG_PROPERTY_ACTION_ON_END_OF_PLAYLIST, Kind::Enum, true, 2},
    {"repeat_single_file_playlists", "Repeat single-track playlists", AIMP_MSG_PROPERTY_REPEAT_SINGLE_FILE_PLAYLISTS, Kind::Boolean, true, 1},
    {"crossmixing", "Cross-mixing", AIMP_MSG_PROPERTY_CROSSMIXING, Kind::Boolean, true, 1},
    {"radio_capture_single_track", "Record one radio track only", AIMP_MSG_PROPERTY_RADIOCAP_SINGLE_TRACK, Kind::Boolean, true, 1},
    {"auto_transition", "Automatic transition effects", AIMP_PLAYER_PROPID_AUTOSWITCHING, Kind::Boolean, false, 1},
    {"auto_crossfade_ms", "Automatic crossfade", AIMP_PLAYER_PROPID_AUTOSWITCHING_CROSSFADE, Kind::Integer, false, INT_MAX},
    {"auto_fade_in_ms", "Automatic fade in", AIMP_PLAYER_PROPID_AUTOSWITCHING_FADEIN, Kind::Integer, false, INT_MAX},
    {"auto_fade_out_ms", "Automatic fade out", AIMP_PLAYER_PROPID_AUTOSWITCHING_FADEOUT, Kind::Integer, false, INT_MAX},
    {"auto_pause_ms", "Pause between tracks", AIMP_PLAYER_PROPID_AUTOSWITCHING_PAUSE_BETWEEN_TRACKS, Kind::Integer, false, INT_MAX},
    {"manual_transition", "Manual transition effects", AIMP_PLAYER_PROPID_MANUALSWITCHING, Kind::Boolean, false, 1},
    {"manual_crossfade_ms", "Manual crossfade", AIMP_PLAYER_PROPID_MANUALSWITCHING_CROSSFADE, Kind::Integer, false, INT_MAX},
    {"manual_fade_in_ms", "Manual fade in", AIMP_PLAYER_PROPID_MANUALSWITCHING_FADEIN, Kind::Integer, false, INT_MAX},
    {"manual_fade_out_ms", "Manual fade out", AIMP_PLAYER_PROPID_MANUALSWITCHING_FADEOUT, Kind::Integer, false, INT_MAX},
};
const Option appOptions[] = {
    {"stay_on_top", "Always on top", AIMP_MSG_PROPERTY_STAYONTOP, Kind::Boolean, true, 1},
    {"reverse_time", "Show remaining time in AIMP", AIMP_MSG_PROPERTY_REVERSETIME, Kind::Boolean, true, 1},
    {"minimized_to_tray", "Minimize AIMP to tray", AIMP_MSG_PROPERTY_MINIMIZED_TO_TRAY, Kind::Boolean, true, 1},
    {"visual_fullscreen", "Fullscreen visualization", AIMP_MSG_PROPERTY_VISUAL_FULLSCREEN, Kind::Boolean, true, 1},
};

HRESULT ReadOption(const Option& o, IAIMPServiceMessageDispatcher* d, IAIMPPropertyList* p, int& value) {
    if (o.message) return d ? d->Send(o.id, AIMP_MSG_PROPVALUE_GET, &value) : E_NOINTERFACE;
    return p ? p->GetValueAsInt32(o.id, &value) : E_NOINTERFACE;
}
HRESULT WriteOption(const Option& o, IAIMPServiceMessageDispatcher* d, IAIMPPropertyList* p, int value) {
    if (o.message) return d ? d->Send(o.id, AIMP_MSG_PROPVALUE_SET, &value) : E_NOINTERFACE;
    return p ? p->SetValueAsInt32(o.id, value) : E_NOINTERFACE;
}
json OptionValue(const Option& o, IAIMPServiceMessageDispatcher* d, IAIMPPropertyList* p) {
    int value = 0;
    if (ReadOption(o, d, p, value) != S_OK) return nullptr;
    if (o.kind == Kind::Boolean) return value != 0;
    return value;
}
template<size_t N> json OptionsState(const Option (&options)[N]) {
    Ref<IAIMPServiceMessageDispatcher> d; Dispatcher(d);
    Ref<IAIMPPropertyList> p; PlayerProperties(p);
    json result = json::object();
    for (const auto& o : options) result[o.key] = OptionValue(o, d.p, p.p);
    return result;
}
template<size_t N> json OptionsCapabilities(const Option (&options)[N]) {
    Ref<IAIMPServiceMessageDispatcher> d; Dispatcher(d);
    Ref<IAIMPPropertyList> p; PlayerProperties(p);
    json result = json::object();
    for (const auto& o : options) {
        json item = {{"label", o.label}, {"type", o.kind == Kind::Boolean ? "boolean" : o.kind == Kind::Enum ? "enum" : "integer"},
                     {"available", !OptionValue(o, d.p, p.p).is_null()}};
        if (o.kind != Kind::Boolean) { item["min"] = 0; item["max"] = o.maximum; }
        if (o.kind == Kind::Integer) { item["unit"] = "ms"; item["step"] = 100; }
        if (std::string(o.key) == "action_on_end_of_track")
            item["choices"] = json::array({{{"id", 0}, {"label", "Normal playback"}}, {{"id", 1}, {"label", "Next track and stop"}}, {{"id", 2}, {"label", "Next track and pause"}}});
        if (std::string(o.key) == "action_on_end_of_playlist")
            item["choices"] = json::array({{{"id", 0}, {"label", "Next playlist"}}, {{"id", 1}, {"label", "Repeat playlist"}}, {{"id", 2}, {"label", "Stop at playlist end"}}});
        result[o.key] = std::move(item);
    }
    return result;
}
template<size_t N> json SetOption(const Option (&options)[N], const json& cmd, const char* name) {
    if (!cmd.contains("parameter") || !cmd["parameter"].is_string() || !cmd.contains("value")) return Failure("INVALID_PARAMETER");
    const auto& key = cmd["parameter"].get_ref<const std::string&>();
    const Option* option = nullptr;
    for (const auto& o : options) if (key == o.key) { option = &o; break; }
    if (!option) return Failure("UNKNOWN_PARAMETER");
    int value;
    if (option->kind == Kind::Boolean) {
        if (!cmd["value"].is_boolean()) return Failure("INVALID_PARAMETER");
        value = cmd["value"].get<bool>() ? 1 : 0;
    } else {
        if (!cmd["value"].is_number_integer() || cmd["value"].get<double>() < 0 || cmd["value"].get<double>() > option->maximum)
            return Failure("INVALID_PARAMETER");
        value = cmd["value"].get<int>();
    }
    Ref<IAIMPServiceMessageDispatcher> d; Dispatcher(d);
    Ref<IAIMPPropertyList> p; PlayerProperties(p);
    int before = 0;
    HRESULT hr = ReadOption(*option, d.p, p.p, before);
    if (hr != S_OK) return Failure("PROPERTY_UNAVAILABLE", hr);
    hr = WriteOption(*option, d.p, p.p, value);
    if (hr != S_OK) return Failure("AIMP_OPERATION_FAILED", hr);
    int after = 0;
    hr = ReadOption(*option, d.p, p.p, after);
    if (hr != S_OK) return Failure("READBACK_FAILED", hr);
    return {{name, OptionsState(options)}};
}

json RecordingState() {
    Ref<IAIMPServiceMessageDispatcher> d;
    BOOL value = FALSE;
    if (Dispatcher(d) != S_OK || !d || d->Send(AIMP_MSG_PROPERTY_RADIOCAP, AIMP_MSG_PROPVALUE_GET, &value) != S_OK)
        return {{"enabled", nullptr}};
    return {{"enabled", value != FALSE}};
}
json LoopState() {
    Ref<IAIMPServiceMessageDispatcher> d;
    json result = {{"stage", nullptr}, {"a", nullptr}, {"b", nullptr}, {"range_available", false}};
    if (Dispatcher(d) != S_OK || !d) return result;
    int stage = 0;
    if (d->Send(AIMP_MSG_PROPERTY_PARTREPEAT, AIMP_MSG_PROPVALUE_GET, &stage) == S_OK && stage >= 0 && stage <= 2) result["stage"] = stage;
    float range[2] = {-1, -1};
    if (d->Send(AIMP_MSG_PROPERTY_PARTREPEAT_RANGE, AIMP_MSG_PROPVALUE_GET, range) == S_OK) {
        result["range_available"] = true;
        if (std::isfinite(range[0]) && range[0] >= 0) result["a"] = range[0];
        if (std::isfinite(range[1]) && range[1] >= 0) result["b"] = range[1];
    }
    return result;
}

json OutputState() {
    json result = {{"current", nullptr}, {"devices", json::array()}, {"list_available", false}};
    Ref<IAIMPPropertyList> p;
    if (PlayerProperties(p) != S_OK || !p) return result;
    Ref<IAIMPString> current;
    if (p->GetValueAsObject(AIMP_PLAYER_PROPID_OUTPUT, IID_IAIMPString, reinterpret_cast<void**>(&current.p)) == S_OK && current)
        result["current"] = WStr(current->GetData());
    Ref<IAIMPObjectList> devices;
    if (p->GetValueAsObject(AIMP_PLAYER_PROPID_OUTPUT, IID_IAIMPObjectList, reinterpret_cast<void**>(&devices.p)) != S_OK || !devices) return result;
    int count = devices->GetCount();
    if (count < 0 || count > 512) return result;
    for (int i = 0; i < count; ++i) {
        Ref<IAIMPString> device;
        if (devices->GetObject(i, IID_IAIMPString, reinterpret_cast<void**>(&device.p)) != S_OK || !device) {
            result["devices"] = json::array();
            return result;
        }
        std::string name = WStr(device->GetData());
        result["devices"].push_back({{"id", name}, {"name", name}});
    }
    result["list_available"] = true;
    return result;
}

struct AppCommand { const char* key; const char* label; int id; int parameter; int maxParameter; bool confirmsPlaylistMutation = false; };
const AppCommand appCommands[] = {
    {"quick_info", "Show playing track info", AIMP_MSG_CMD_QFI_PLAYING_TRACK, 0, 0},
    {"about", "Show About AIMP", AIMP_MSG_CMD_ABOUT, 0, 0},
    {"options", "Show AIMP settings", AIMP_MSG_CMD_OPTIONS, 0, 0},
    {"plugins", "Show plug-in settings", AIMP_MSG_CMD_PLUGINS, 0, 0},
    {"scheduler", "Show scheduler settings", AIMP_MSG_CMD_SCHEDULER, 0, 0},
    {"visual_next", "Next visualization", AIMP_MSG_CMD_VISUAL_NEXT, 0, 0},
    {"visual_prev", "Previous visualization", AIMP_MSG_CMD_VISUAL_PREV, 0, 0},
    {"visual_start", "Start visualization", AIMP_MSG_CMD_VISUAL_START, 0, 0},
    {"visual_stop", "Stop visualization", AIMP_MSG_CMD_VISUAL_STOP, 0, 0},
    {"play_previous_playlist", "Play previous playlist", AIMP_MSG_CMD_PLAY_PREV_PLAYLIST, 0, 0},
    {"open_files", "Open files dialog", AIMP_MSG_CMD_OPEN_FILES, 0, 0},
    {"open_folders", "Open folders dialog", AIMP_MSG_CMD_OPEN_FOLDERS, 0, 0},
    {"open_playlists", "Open playlists dialog", AIMP_MSG_CMD_OPEN_PLAYLISTS, 0, 0},
    {"save_playlist", "Save playlist dialog", AIMP_MSG_CMD_SAVE_PLAYLISTS, 0, 0},
    {"bookmarks", "Show bookmarks", AIMP_MSG_CMD_BOOKMARKS, 0, 0},
    {"bookmark_playing", "Bookmark playing track", AIMP_MSG_CMD_BOOKMARKS_ADD, 0, 0},
    {"bookmark_selected", "Bookmark selected tracks", AIMP_MSG_CMD_BOOKMARKS_ADD, 1, 0},
    {"rescan_tags", "Rescan active playlist tags", AIMP_MSG_CMD_PLS_RESCAN, 0, 0},
    {"rescan_selected_tags", "Rescan selected track tags", AIMP_MSG_CMD_PLS_RESCAN_SELECTED, 0, 0},
    {"focus_playing", "Focus playing track", AIMP_MSG_CMD_PLS_FOCUS_PLAYING, 0, 0},
    {"enable_selected", "Enable selected tracks", AIMP_MSG_CMD_PLS_SWITCH_ON, 0, 0},
    {"disable_selected", "Disable selected tracks", AIMP_MSG_CMD_PLS_SWITCH_OFF, 0, 0},
    {"add_files", "Add files dialog", AIMP_MSG_CMD_ADD_FILES, 0, 0},
    {"add_folders", "Add folders dialog", AIMP_MSG_CMD_ADD_FOLDERS, 0, 0},
    {"add_playlists", "Add playlists dialog", AIMP_MSG_CMD_ADD_PLAYLISTS, 0, 0},
    {"add_url", "Add URL dialog", AIMP_MSG_CMD_ADD_URL, 0, 0},
    {"quick_tag_editor", "Edit playing track tags", AIMP_MSG_CMD_QTE_PLAYING_TRACK, 0, 0},
    {"search", "Show advanced search", AIMP_MSG_CMD_SEARCH, 0, 0},
    {"dsp_manager", "Show DSP manager", AIMP_MSG_CMD_DSPMANAGER, 0, 3},
    {"cleanup_missing", "Remove missing entries from active playlist", AIMP_MSG_CMD_PLS_DELETE_NON_EXISTS, 0, 0, true},
    {"cleanup_non_selected", "Remove unselected entries from active playlist", AIMP_MSG_CMD_PLS_DELETE_NON_SELECTED, 0, 0, true},
    {"cleanup_selected", "Remove selected entries from active playlist", AIMP_MSG_CMD_PLS_DELETE_SELECTED, 0, 0, true},
    {"cleanup_disabled", "Remove disabled entries from active playlist", AIMP_MSG_CMD_PLS_DELETE_SWITCHEDOFF, 0, 0, true},
    {"cleanup_duplicates", "Remove duplicate entries from active playlist", AIMP_MSG_CMD_PLS_DELETE_DUPLICATES, 0, 0, true},
    {"sync_preimage", "Synchronize active playlist with its source", AIMP_MSG_CMD_PLS_RELOAD_FROM_PREIMAGE, 0, 0, true},
};

std::string ActivePlaylistGuid() {
    Ref<IAIMPServicePlaylistManager> manager;
    Ref<IAIMPPlaylist> playlist;
    Ref<IAIMPPropertyList> properties;
    Ref<IAIMPString> id;
    if (!g_core || g_core->QueryInterface(IID_IAIMPServicePlaylistManager, reinterpret_cast<void**>(&manager.p)) != S_OK || !manager ||
        manager->GetActivePlaylist(&playlist.p) != S_OK || !playlist ||
        playlist->QueryInterface(IID_IAIMPPropertyList, reinterpret_cast<void**>(&properties.p)) != S_OK || !properties ||
        properties->GetValueAsObject(AIMP_PLAYLIST_PROPID_ID, IID_IAIMPString, reinterpret_cast<void**>(&id.p)) != S_OK || !id)
        return {};
    const int length = id->GetLength();
    return length > 0 && length <= 512 && id->GetData() ? WStrToUtf8(std::wstring(id->GetData(), length)) : std::string{};
}

// Message dispatcher exposes only synchronous Send. Defer UI work to our own
// message window so a modal AIMP dialog cannot block the WebSocket reply that
// acknowledges the request. Only one UI command may be pending/executing.
constexpr UINT UiCommandMessage = WM_APP + 97;
constexpr wchar_t UiClass[] = L"AimpCompanionControl.UI.Commands.v2";
HWND uiWindow = nullptr;
bool uiBusy = false;
json lastUiCommand = nullptr;
std::string queuedPlaylistGuid;

LRESULT CALLBACK UiProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == UiCommandMessage) {
        UiCallbackLease lease;
        if (!lease) {
            lastUiCommand = {{"status", "failed"}, {"error", "MODULE_LIFETIME_UNAVAILABLE"}};
            uiBusy = false; return 0;
        }
        try {
            if (!uiBusy || !g_core || wParam >= sizeof(appCommands) / sizeof(appCommands[0])) return 0;
            const auto& command = appCommands[wParam];
            // UI commands are deferred. Never remove entries from a different
            // active playlist if the user switches tabs before dispatch.
            if (command.confirmsPlaylistMutation && (queuedPlaylistGuid.empty() || ActivePlaylistGuid() != queuedPlaylistGuid)) {
                lastUiCommand = {{"command", command.key}, {"status", "failed"}, {"error", "ACTIVE_PLAYLIST_CHANGED"}};
                uiBusy = false;
                queuedPlaylistGuid.clear();
                return 0;
            }
            lastUiCommand = {{"command", command.key}, {"status", "executing"}};
            Ref<IAIMPServiceMessageDispatcher> d;
            HRESULT hr = Dispatcher(d);
            if (hr == S_OK && d) hr = d->Send(AIMP_MSG_CMD_STATE_GET, command.id, nullptr);
            if (hr == S_OK) hr = d ? d->Send(command.id, static_cast<int>(lParam), nullptr) : E_NOINTERFACE;
            if (!lease.current()) return 0;
            lastUiCommand = {{"command", command.key}, {"status", hr == S_OK ? "completed" : "failed"}, {"hresult", static_cast<long>(hr)}};
        } catch (...) {
            if (lease.current()) lastUiCommand = {{"status", "failed"}, {"error", "AIMP_UI_EXCEPTION"}};
        }
        if (!lease.current()) return 0;
        uiBusy = false;
        queuedPlaylistGuid.clear();
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}
bool EnsureUiWindow() {
    if (uiWindow) return true;
    if (GetCurrentThreadId() != g_mainThreadId) return false;
    WNDCLASSW wc{};
    wc.lpfnWndProc = UiProc; wc.hInstance = g_hInstance; wc.lpszClassName = UiClass;
    if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
    uiWindow = CreateWindowExW(0, UiClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, g_hInstance, nullptr);
    return uiWindow != nullptr;
}
json QueueAppCommand(const json& cmd) {
    if (!cmd.contains("command") || !cmd["command"].is_string()) return Failure("INVALID_PARAMETER");
    const auto& key = cmd["command"].get_ref<const std::string&>();
    size_t index = 0;
    while (index < sizeof(appCommands) / sizeof(appCommands[0]) && key != appCommands[index].key) ++index;
    if (index == sizeof(appCommands) / sizeof(appCommands[0])) return Failure("UNKNOWN_APP_COMMAND");
    const auto& command = appCommands[index];
    if (command.confirmsPlaylistMutation &&
        (!cmd.contains("confirm") || !cmd["confirm"].is_boolean() || !cmd["confirm"].get<bool>())) return Failure("CONFIRMATION_REQUIRED");
    int param = command.parameter;
    if (cmd.contains("param")) {
        if (command.maxParameter == 0 || !cmd["param"].is_number_integer() || cmd["param"].get<double>() < 0 || cmd["param"].get<double>() > command.maxParameter)
            return Failure("INVALID_PARAMETER");
        param = cmd["param"].get<int>();
    }
    if (uiBusy) return Failure("APP_COMMAND_BUSY");
    Ref<IAIMPServiceMessageDispatcher> d;
    if (Dispatcher(d) != S_OK || !d) return Failure("MESSAGE_DISPATCHER_UNAVAILABLE");
    HRESULT hr = d->Send(AIMP_MSG_CMD_STATE_GET, command.id, nullptr);
    if (hr != S_OK) return Failure("APP_COMMAND_DISABLED", hr);
    std::string playlistGuid;
    if (command.confirmsPlaylistMutation) {
        playlistGuid = ActivePlaylistGuid();
        if (playlistGuid.empty()) return Failure("ACTIVE_PLAYLIST_UNAVAILABLE");
    }
    if (!EnsureUiWindow()) return Failure("UI_QUEUE_UNAVAILABLE", HRESULT_FROM_WIN32(GetLastError()));
    uiBusy = true;
    queuedPlaylistGuid = playlistGuid;
    lastUiCommand = {{"command", key}, {"status", "queued"}};
    if (command.confirmsPlaylistMutation) lastUiCommand["playlist_guid"] = playlistGuid;
    if (!PostMessageW(uiWindow, UiCommandMessage, static_cast<WPARAM>(index), static_cast<LPARAM>(param))) {
        uiBusy = false;
        queuedPlaylistGuid.clear();
        HRESULT hr = HRESULT_FROM_WIN32(GetLastError());
        lastUiCommand = {{"command", key}, {"status", "failed"}, {"hresult", static_cast<long>(hr)}};
        return Failure("UI_QUEUE_FAILED", hr);
    }
    return {{"acknowledgement", "queued"}, {"last_app_command", lastUiCommand}};
}
}

json GetPlayerExtensionCapabilities() {
    json loop = LoopState(), output = OutputState();
    Ref<IAIMPServiceMessageDispatcher> d; Dispatcher(d);
    json commands = json::object();
    for (const auto& c : appCommands) {
        json item = {{"label", c.label}, {"available", bool(d)}, {"enabled", d && d->Send(AIMP_MSG_CMD_STATE_GET, c.id, nullptr) == S_OK}, {"acknowledgement", "queued"}};
        if (c.maxParameter) { item["param_min"] = 0; item["param_max"] = c.maxParameter; }
        if (c.confirmsPlaylistMutation) {
            item["requires_confirmation"] = true;
            item["scope"] = "active_playlist";
            item["deletes_disk_files"] = false;
        }
        commands[c.key] = std::move(item);
    }
    return {{"player_options", OptionsCapabilities(playerOptions)}, {"app_options", OptionsCapabilities(appOptions)},
            {"recording", {{"available", !RecordingState()["enabled"].is_null()}}},
            {"loop", {{"available", !loop["stage"].is_null()}, {"range", loop["range_available"]}, {"cycle", bool(d)}}},
            {"output", {{"available", !output["current"].is_null()}, {"list", output["list_available"]}}},
            {"app_commands", commands}};
}

json GetPlayerExtensionState() {
    return {{"player_options", OptionsState(playerOptions)}, {"app_options", OptionsState(appOptions)},
            {"recording", RecordingState()}, {"loop", LoopState()}, {"output", OutputState()}, {"last_app_command", lastUiCommand}};
}

bool HandlePlayerExtensionCommand(const std::string& action, const json& cmd, json& rsp) {
    if (action == "player/options/get") rsp = {{"player_options", OptionsState(playerOptions)}};
    else if (action == "player/options/set") rsp = SetOption(playerOptions, cmd, "player_options");
    else if (action == "app/options/get") rsp = {{"app_options", OptionsState(appOptions)}};
    else if (action == "app/options/set") rsp = SetOption(appOptions, cmd, "app_options");
    else if (action == "app/command") rsp = QueueAppCommand(cmd);
    else if (action == "recording/get") rsp = {{"recording", RecordingState()}};
    else if (action == "recording/set") {
        if (!cmd.contains("enabled") || !cmd["enabled"].is_boolean()) { rsp = Failure("INVALID_PARAMETER"); return true; }
        Ref<IAIMPServiceMessageDispatcher> d;
        HRESULT hr = Dispatcher(d);
        BOOL enabled = cmd["enabled"].get<bool>() ? TRUE : FALSE;
        if (hr == S_OK && d) hr = d->Send(AIMP_MSG_PROPERTY_RADIOCAP, AIMP_MSG_PROPVALUE_SET, &enabled);
        if (hr != S_OK) rsp = Failure("AIMP_OPERATION_FAILED", hr);
        else {
            json state = RecordingState();
            rsp = state["enabled"].is_null() ? Failure("READBACK_FAILED") : json{{"recording", state}};
        }
    } else if (action == "loop/get") rsp = {{"loop", LoopState()}};
    else if (action == "loop/cycle" || action == "loop/set") {
        Ref<IAIMPServiceMessageDispatcher> d;
        HRESULT hr = Dispatcher(d);
        if (hr != S_OK || !d) { rsp = Failure("MESSAGE_DISPATCHER_UNAVAILABLE", hr); return true; }
        if (action == "loop/cycle") {
            hr = d->Send(AIMP_MSG_CMD_TOGGLE_PARTREPEAT, 0, nullptr);
        } else {
            float range[2] = {-1, -1};
            const char* keys[] = {"a", "b"};
            for (int i = 0; i < 2; ++i) {
                if (!cmd.contains(keys[i])) { rsp = Failure("INVALID_LOOP_RANGE"); return true; }
                const auto& value = cmd[keys[i]];
                if (value.is_null()) continue;
                if (!value.is_number() || !std::isfinite(value.get<double>()) || value.get<double>() < 0 || value.get<double>() > 1e9) {
                    rsp = Failure("INVALID_LOOP_RANGE"); return true;
                }
                range[i] = value.get<float>();
            }
            if ((range[0] < 0 && range[1] >= 0) || (range[1] >= 0 && range[1] <= range[0])) { rsp = Failure("INVALID_LOOP_RANGE"); return true; }
            if (range[0] >= 0) {
                Ref<IAIMPServicePlayer> player;
                double duration = 0;
                HRESULT playerHr = g_core->QueryInterface(IID_IAIMPServicePlayer, reinterpret_cast<void**>(&player.p));
                if (playerHr != S_OK || !player || player->GetDuration(&duration) != S_OK || !std::isfinite(duration) || duration <= 0) {
                    rsp = Failure("NO_SEEKABLE_TRACK"); return true;
                }
                if (range[0] >= duration || range[1] > duration + 0.001) { rsp = Failure("LOOP_OUTSIDE_TRACK"); return true; }
            }
            hr = d->Send(AIMP_MSG_PROPERTY_PARTREPEAT_RANGE, AIMP_MSG_PROPVALUE_SET, range);
        }
        if (hr != S_OK) rsp = Failure("AIMP_OPERATION_FAILED", hr);
        else {
            json loop = LoopState();
            rsp = loop["stage"].is_null() ? Failure("READBACK_FAILED") : json{{"loop", loop}};
        }
    } else if (action == "output/get") rsp = {{"output", OutputState()}};
    else if (action == "output/set") {
        if (!cmd.contains("id") || !cmd["id"].is_string() || cmd["id"].get_ref<const std::string&>().empty() || cmd["id"].get_ref<const std::string&>().size() > 4096) {
            rsp = Failure("INVALID_OUTPUT"); return true;
        }
        Ref<IAIMPPropertyList> p;
        HRESULT hr = PlayerProperties(p);
        Ref<IAIMPObjectList> devices;
        if (hr == S_OK && p) hr = p->GetValueAsObject(AIMP_PLAYER_PROPID_OUTPUT, IID_IAIMPObjectList, reinterpret_cast<void**>(&devices.p));
        if (hr != S_OK || !devices) { rsp = Failure("OUTPUT_UNAVAILABLE", hr); return true; }
        int count = devices->GetCount();
        if (count < 0 || count > 512) { rsp = Failure("OUTPUT_LIST_TOO_LARGE"); return true; }
        const auto& id = cmd["id"].get_ref<const std::string&>();
        bool found = false;
        for (int i = 0; i < count; ++i) {
            Ref<IAIMPString> device;
            hr = devices->GetObject(i, IID_IAIMPString, reinterpret_cast<void**>(&device.p));
            if (hr != S_OK || !device) continue;
            if (WStr(device->GetData()) != id) continue;
            found = true;
            hr = p->SetValueAsObject(AIMP_PLAYER_PROPID_OUTPUT, device.p);
            break;
        }
        if (!found) rsp = Failure("OUTPUT_NOT_FOUND");
        else if (hr != S_OK) rsp = Failure("AIMP_OPERATION_FAILED", hr);
        else {
            json output = OutputState();
            rsp = output["current"].is_null() ? Failure("READBACK_FAILED") : json{{"output", output}};
        }
    } else return false;
    return true;
}

void FinalizePlayerExtensions() {
    if (uiWindow) { DestroyWindow(uiWindow); uiWindow = nullptr; }
    UnregisterClassW(UiClass, g_hInstance);
    uiBusy = false;
    queuedPlaylistGuid.clear();
    lastUiCommand = nullptr;
}
