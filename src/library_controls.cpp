#include "library_controls.h"
#include "globals.h"
#include "utils.h"
#include "apiTagEditor.h"
#include "library_tag_catalog.h"
#include <cmath>
#include <limits>

namespace {
constexpr int kMaxPlaylists = 4096;
constexpr int kMaxQueuePage = 250;
constexpr int kMaxQueueClear = 10000;

template<class T> struct Ref {
    T* p = nullptr;
    Ref() = default;
    Ref(const Ref&) = delete;
    Ref& operator=(const Ref&) = delete;
    ~Ref() { if (p) p->Release(); }
    T* operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
    T** put() { return &p; }
    void** out() { return reinterpret_cast<void**>(&p); }
};
template<class T> bool Service(REFIID iid, Ref<T>& result) {
    return g_core && SUCCEEDED(g_core->QueryInterface(iid, result.out())) && result;
}
json Error(const char* code, const std::string& message = "") {
    json result = {{"error", {{"code", code}}}};
    if (!message.empty()) result["error"]["message"] = message;
    return result;
}
bool Check(HRESULT result, json& response, const char* operation) {
    if (SUCCEEDED(result)) return true;
    response = Error("AIMP_OPERATION_FAILED", operation);
    response["error"]["hresult"] = static_cast<int32_t>(result);
    return false;
}
bool Integer(const json& cmd, const char* key, int& value, int minimum, int maximum, bool required = true) {
    if (!cmd.contains(key)) return !required;
    if (!cmd[key].is_number_integer()) return false;
    const double n = cmd[key].get<double>();
    if (n < minimum || n > maximum) return false;
    value = cmd[key].get<int>();
    return true;
}
bool Boolean(const json& cmd, const char* key, bool& value, bool required = true) {
    if (!cmd.contains(key)) return !required;
    if (!cmd[key].is_boolean()) return false;
    value = cmd[key].get<bool>();
    return true;
}
bool Text(const json& cmd, const char* key, std::string& value, size_t maxLength) {
    if (!cmd.contains(key) || !cmd[key].is_string()) return false;
    value = cmd[key].get<std::string>();
    return !value.empty() && value.size() <= maxLength && value.find('\0') == std::string::npos;
}
std::string ReadText(IAIMPPropertyList* object, int property, size_t cap = 512) {
    Ref<IAIMPString> value;
    if (FAILED(object->GetValueAsObject(property, IID_IAIMPString, value.out())) || !value) return "";
    const auto* data = value->GetData();
    if (!data) return "";
    size_t size = std::min(cap, static_cast<size_t>(std::max(0, value->GetLength())));
    // Never leave a dangling UTF-16 high surrogate after truncation.
    if (size && data[size - 1] >= 0xd800 && data[size - 1] <= 0xdbff) --size;
    return WStrToUtf8(std::wstring(data, size));
}
bool MakeString(const std::string& text, Ref<IAIMPString>& result, json& response) {
    if (!g_core || !Check(g_core->CreateObject(IID_IAIMPString, result.out()), response, "Create AIMP string") || !result) {
        if (!response.contains("error")) response = Error("AIMP_UNAVAILABLE");
        return false;
    }
    auto wide = Utf8ToWStr(text);
    if (wide.empty() && !text.empty()) { response = Error("INVALID_UTF8"); return false; }
    return Check(result->SetData(wide.data(), static_cast<int>(wide.size())), response, "Set AIMP string");
}
bool Properties(IUnknown* object, Ref<IAIMPPropertyList>& props, json& response) {
    if (!object || !Check(object->QueryInterface(IID_IAIMPPropertyList, props.out()), response, "Read object properties") || !props) {
        if (!response.contains("error")) response = Error("PROPERTIES_UNAVAILABLE");
        return false;
    }
    return true;
}
std::string PlaylistGuid(IAIMPPlaylist* playlist) {
    Ref<IAIMPPropertyList> props;
    if (FAILED(playlist->QueryInterface(IID_IAIMPPropertyList, props.out())) || !props) return "";
    return ReadText(props.p, AIMP_PLAYLIST_PROPID_ID);
}
int PlaylistIndex(IAIMPServicePlaylistManager* manager, IAIMPPlaylist* target) {
    const auto guid = PlaylistGuid(target);
    const int count = std::min(manager->GetLoadedPlaylistCount(), kMaxPlaylists);
    for (int i = 0; i < count; ++i) {
        Ref<IAIMPPlaylist> candidate;
        if (SUCCEEDED(manager->GetLoadedPlaylist(i, candidate.put())) && candidate &&
            (candidate.p == target || (!guid.empty() && PlaylistGuid(candidate.p) == guid))) return i;
    }
    return -1;
}
bool Manager(Ref<IAIMPServicePlaylistManager>& manager, json& response) {
    if (Service(IID_IAIMPServicePlaylistManager, manager)) return true;
    response = Error("PLAYLISTS_UNAVAILABLE"); return false;
}
bool ResolvePlaylist(IAIMPServicePlaylistManager* manager, const json& cmd, Ref<IAIMPPlaylist>& playlist,
                     int& index, json& response, const char* defaultTarget = "active") {
    std::string target = cmd.contains("playlist_id") ? "explicit" : defaultTarget;
    if (cmd.contains("target")) {
        if (!cmd["target"].is_string()) { response = Error("INVALID_TARGET"); return false; }
        target = cmd["target"].get<std::string>();
    }
    HRESULT hr = E_INVALIDARG;
    if (target == "explicit") {
        if (!Integer(cmd, "playlist_id", index, 0, std::numeric_limits<int>::max())) {
            response = Error("INVALID_PLAYLIST_ID"); return false;
        }
        if (index >= manager->GetLoadedPlaylistCount()) { response = Error("PLAYLIST_NOT_FOUND"); return false; }
        hr = manager->GetLoadedPlaylist(index, playlist.put());
    } else if (target == "active") {
        hr = manager->GetActivePlaylist(playlist.put());
    } else if (target == "playing") {
        hr = manager->GetPlayingPlaylist(playlist.put());
    } else if (target == "focused") {
        // Extended commands operate on current AIMP focus. The navigation cache
        // defaults to index zero and must never supply a destructive target.
        hr = manager->GetActivePlaylist(playlist.put());
        if (hr != S_OK || !playlist) { response = Error("NO_FOCUSED_PLAYLIST"); return false; }
    } else { response = Error("INVALID_TARGET"); return false; }
    if (FAILED(hr) || !playlist) { response = Error("PLAYLIST_NOT_FOUND"); return false; }
    if (target == "active" || target == "playing" || target == "focused") index = PlaylistIndex(manager, playlist.p);
    if (target == "focused" && index < 0) { response = Error("NO_FOCUSED_PLAYLIST"); return false; }
    return true;
}
bool ResolveTrack(IAIMPServicePlaylistManager* manager, const json& cmd, Ref<IAIMPPlaylist>& playlist,
                  Ref<IAIMPPlaylistItem>& item, int& playlistIndex, int& trackIndex, json& response) {
    std::string target = (cmd.contains("playlist_id") || cmd.contains("track_id")) ? "explicit" : "focused";
    if (cmd.contains("target")) {
        if (!cmd["target"].is_string()) { response = Error("INVALID_TARGET"); return false; }
        target = cmd["target"].get<std::string>();
    }
    if (target != "explicit" && target != "focused" && target != "playing") {
        response = Error("INVALID_TARGET"); return false;
    }
    if (target == "playing") {
        Ref<IAIMPServicePlayer> player;
        if (!Service(IID_IAIMPServicePlayer, player) || FAILED(player->GetPlaylistItem(item.put())) || !item) {
            response = Error("NO_PLAYING_TRACK"); return false;
        }
        if (!Check(item->GetValueAsObject(AIMP_PLAYLISTITEM_PROPID_PLAYLIST, IID_IAIMPPlaylist, playlist.out()),
                   response, "Read playing track playlist") || !playlist) return false;
        if (!Check(item->GetValueAsInt32(AIMP_PLAYLISTITEM_PROPID_INDEX, &trackIndex), response, "Read playing track index")) return false;
        playlistIndex = PlaylistIndex(manager, playlist.p);
        return true;
    }
    json selection = cmd;
    selection["target"] = target;
    if (!ResolvePlaylist(manager, selection, playlist, playlistIndex, response, "focused")) return false;
    if (target == "explicit") {
        if (!Integer(cmd, "track_id", trackIndex, 0, std::numeric_limits<int>::max())) {
            response = Error("INVALID_TRACK_ID"); return false;
        }
    } else {
        Ref<IAIMPPropertyList> props;
        HRESULT hr = playlist->QueryInterface(IID_IAIMPPropertyList, props.out());
        if (hr == S_OK && props) hr = props->GetValueAsInt32(AIMP_PLAYLIST_PROPID_FOCUSINDEX, &trackIndex);
        // -1 is the SDK's explicit "no focused item" state, including group
        // focus. Do not silently turn it into the first playlist entry.
        if (hr != S_OK || !props || trackIndex < 0 || trackIndex >= playlist->GetItemCount()) {
            response = Error("NO_FOCUSED_TRACK", "Focus a track in the active AIMP playlist before using this action");
            if (hr != S_OK) response["error"]["hresult"] = static_cast<int32_t>(hr);
            return false;
        }
    }
    if (trackIndex < 0 || trackIndex >= playlist->GetItemCount()) { response = Error("TRACK_NOT_FOUND"); return false; }
    if (!Check(playlist->GetItem(trackIndex, IID_IAIMPPlaylistItem, item.out()), response, "Read playlist track") || !item) {
        if (!response.contains("error")) response = Error("TRACK_NOT_FOUND");
        return false;
    }
    return true;
}
void ReadInfo(IAIMPFileInfo* info, json& output, bool full) {
    struct Field { const char* name; int property; };
    const Field texts[] = {
        {"title", AIMP_FILEINFO_PROPID_TITLE}, {"artist", AIMP_FILEINFO_PROPID_ARTIST},
        {"album", AIMP_FILEINFO_PROPID_ALBUM}, {"file_path", AIMP_FILEINFO_PROPID_FILENAME},
        {"album_artist", AIMP_FILEINFO_PROPID_ALBUMARTIST}, {"genre", AIMP_FILEINFO_PROPID_GENRE},
        {"year", AIMP_FILEINFO_PROPID_DATE}, {"track_number", AIMP_FILEINFO_PROPID_TRACKNUMBER},
        {"track_total", AIMP_FILEINFO_PROPID_TRACKTOTAL}, {"disc_number", AIMP_FILEINFO_PROPID_DISKNUMBER},
        {"disc_total", AIMP_FILEINFO_PROPID_DISKTOTAL}, {"composer", AIMP_FILEINFO_PROPID_COMPOSER},
        {"copyright", AIMP_FILEINFO_PROPID_COPYRIGHT}, {"codec", AIMP_FILEINFO_PROPID_CODEC},
        {"conductor", AIMP_FILEINFO_PROPID_CONDUCTOR}, {"mood", AIMP_FILEINFO_PROPID_MOOD},
        {"catalog", AIMP_FILEINFO_PROPID_CATALOG}, {"isrc", AIMP_FILEINFO_PROPID_ISRC},
        {"lyricist", AIMP_FILEINFO_PROPID_LYRICIST}, {"encoded_by", AIMP_FILEINFO_PROPID_ENCODEDBY},
        {"publisher", AIMP_FILEINFO_PROPID_PUBLISHER},
        {"key", AIMP_FILEINFO_PROPID_KEY}, {"labels", AIMP_FILEINFO_PROPID_ML_LABELS},
        {"url", AIMP_FILEINFO_PROPID_URL}, {"comment", AIMP_FILEINFO_PROPID_COMMENT},
        {"lyrics", AIMP_FILEINFO_PROPID_LYRICS}
    };
    for (size_t i = 0; i < sizeof(texts) / sizeof(texts[0]); ++i) {
        if (!full && i >= 4) break;
        const auto& field = texts[i];
        output[field.name] = ReadText(info, field.property, full ? 4096 : 256);
    }
    const Field ints[] = {
        {"bitrate", AIMP_FILEINFO_PROPID_BITRATE}, {"bpm", AIMP_FILEINFO_PROPID_BPM},
        {"channels", AIMP_FILEINFO_PROPID_CHANNELS}, {"sample_rate", AIMP_FILEINFO_PROPID_SAMPLERATE},
        {"bit_depth", AIMP_FILEINFO_PROPID_BITDEPTH}, {"tag_rating", AIMP_FILEINFO_PROPID_RATING},
        {"play_count", AIMP_FILEINFO_PROPID_ML_PLAYCOUNT}
    };
    if (full) for (const auto& field : ints) {
        int value = 0;
        if (SUCCEEDED(info->GetValueAsInt32(field.property, &value))) output[field.name] = value;
    }
    const Field floats[] = {
        {"duration", AIMP_FILEINFO_PROPID_DURATION}, {"track_gain", AIMP_FILEINFO_PROPID_TRACKGAIN},
        {"track_peak", AIMP_FILEINFO_PROPID_TRACKPEAK}, {"album_gain", AIMP_FILEINFO_PROPID_ALBUMGAIN},
        {"album_peak", AIMP_FILEINFO_PROPID_ALBUMPEAK}, {"library_rating", AIMP_FILEINFO_PROPID_ML_RATING},
        {"library_display_mark", AIMP_FILEINFO_PROPID_ML_DISPLAYING_MARK},
        // The official IAIMPFileInfo documentation specifies Double here (header comment says Int32).
        {"library_mark", AIMP_FILEINFO_PROPID_ML_MARK},
        {"library_added_tdatetime", AIMP_FILEINFO_PROPID_ML_ADDINGDATE},
        {"last_played_tdatetime", AIMP_FILEINFO_PROPID_ML_LASTPLAYDATE}
    };
    for (size_t i = 0; i < sizeof(floats) / sizeof(floats[0]); ++i) {
        if (!full && i) break;
        double value = 0;
        if (SUCCEEDED(info->GetValueAsFloat(floats[i].property, &value)) && std::isfinite(value)) output[floats[i].name] = value;
    }
    INT64 bytes = 0;
    if (full && SUCCEEDED(info->GetValueAsInt64(AIMP_FILEINFO_PROPID_FILESIZE, &bytes))) output["size_bytes"] = bytes;
}
json TrackInfo(IAIMPPlaylistItem* item, int playlistIndex, int trackIndex, bool full) {
    json output = {{"playlist_id", playlistIndex}, {"track_id", trackIndex}, {"id", trackIndex}};
    Ref<IAIMPFileInfo> info;
    if (SUCCEEDED(item->GetValueAsObject(AIMP_PLAYLISTITEM_PROPID_FILEINFO, IID_IAIMPFileInfo, info.out())) && info)
        ReadInfo(info.p, output, full);
    if (!output.contains("title") || output["title"] == "")
        output["title"] = ReadText(item, AIMP_PLAYLISTITEM_PROPID_DISPLAYTEXT, full ? 4096 : 256);
    if (!output.contains("file_path") || output["file_path"] == "")
        output["file_path"] = ReadText(item, AIMP_PLAYLISTITEM_PROPID_FILENAME, full ? 4096 : 256);
    int value = 0;
    if (SUCCEEDED(item->GetValueAsInt32(AIMP_PLAYLISTITEM_PROPID_PLAYINGSWITCH, &value))) output["enabled"] = value != 0;
    if (SUCCEEDED(item->GetValueAsInt32(AIMP_PLAYLISTITEM_PROPID_SELECTED, &value))) output["selected"] = value != 0;
    if (SUCCEEDED(item->GetValueAsInt32(AIMP_PLAYLISTITEM_PROPID_PLAYBACKQUEUEINDEX, &value))) output["queue_index"] = value;
    double mark = 0;
    if (SUCCEEDED(item->GetValueAsFloat(AIMP_PLAYLISTITEM_PROPID_MARK, &mark)) && std::isfinite(mark)) output["rating"] = mark;
    return output;
}
json PlaylistInfo(IAIMPPlaylist* playlist, int index) {
    json output = {{"id", index}, {"track_count", playlist->GetItemCount()}};
    Ref<IAIMPPropertyList> props;
    if (FAILED(playlist->QueryInterface(IID_IAIMPPropertyList, props.out())) || !props) return output;
    output["aimp_id"] = ReadText(props.p, AIMP_PLAYLIST_PROPID_ID);
    output["name"] = ReadText(props.p, AIMP_PLAYLIST_PROPID_NAME);
    int readOnly = 0;
    if (SUCCEEDED(props->GetValueAsInt32(AIMP_PLAYLIST_PROPID_READONLY, &readOnly))) output["read_only"] = readOnly != 0;
    double seconds = 0;
    if (SUCCEEDED(props->GetValueAsFloat(AIMP_PLAYLIST_PROPID_DURATION, &seconds)) && std::isfinite(seconds)) output["duration"] = seconds;
    if (SUCCEEDED(props->GetValueAsFloat(AIMP_PLAYLIST_PROPID_DURATION_REMAINING, &seconds)) && std::isfinite(seconds)) output["remaining_duration"] = seconds;
    INT64 bytes = 0;
    if (SUCCEEDED(props->GetValueAsInt64(AIMP_PLAYLIST_PROPID_SIZE, &bytes))) output["size_bytes"] = bytes;
    return output;
}
struct SortMode { const char* id; const char* label; int value; };
constexpr SortMode sorts[] = {
    {"title", "Title", AIMP_PLAYLIST_SORTMODE_TITLE}, {"filename", "File name", AIMP_PLAYLIST_SORTMODE_FILENAME},
    {"duration", "Duration", AIMP_PLAYLIST_SORTMODE_DURATION}, {"artist", "Artist", AIMP_PLAYLIST_SORTMODE_ARTIST},
    {"reverse", "Reverse order", AIMP_PLAYLIST_SORTMODE_INVERSE}, {"random", "Randomize tracks", AIMP_PLAYLIST_SORTMODE_RANDOMIZE},
    {"random_groups", "Randomize groups", AIMP_PLAYLIST_SORTMODE_RANDOMIZE_GROUPS},
    {"random_group_items", "Randomize tracks within groups", AIMP_PLAYLIST_SORTMODE_RANDOMIZE_GROUPITEMS},
    {"random_all", "Randomize groups and tracks", AIMP_PLAYLIST_SORTMODE_RANDOMIZE_GROUPS_AND_IT_ITEMS}
};
const std::set<std::string> playlistCommands = {"playlists/info", "playlists/create", "playlists/rename", "playlists/close", "playlists/add", "playlists/clear", "playlists/sort"};
const std::set<std::string> trackCommands = {"playlists/track/info", "playlists/track/remove", "playlists/track/move", "playlists/track/enabled", "playlists/track/rating"};
const std::set<std::string> queueCommands = {"queue/get", "queue/add", "queue/remove", "queue/clear", "queue/move", "queue/suspended"};

bool QueueCommand(const std::string& action, const json& cmd, json& response) {
    Ref<IAIMPServicePlaylistManager> manager;
    if (!Manager(manager, response)) return true;
    Ref<IAIMPPlaylistQueue> queue;
    // This interface is exposed by the playlist manager, not by IAIMPCore.
    if (FAILED(manager->QueryInterface(IID_IAIMPPlaylistQueue, queue.out())) || !queue) {
        response = Error("QUEUE_UNAVAILABLE"); return true;
    }
    const int count = queue->GetItemCount();
    if (count < 0) { response = Error("INVALID_QUEUE_STATE"); return true; }
    if (action == "queue/get") {
        int offset = 0, limit = 100;
        if (!Integer(cmd, "offset", offset, 0, std::numeric_limits<int>::max(), false) ||
            !Integer(cmd, "limit", limit, 1, kMaxQueuePage, false)) { response = Error("INVALID_QUEUE_PAGE"); return true; }
        json data = {{"count", count}, {"offset", offset}, {"limit", limit}, {"items", json::array()}};
        Ref<IAIMPPropertyList> props;
        if (SUCCEEDED(queue->QueryInterface(IID_IAIMPPropertyList, props.out())) && props) {
            int suspended = 0;
            if (SUCCEEDED(props->GetValueAsInt32(AIMP_PLAYLISTQUEUE_PROPID_SUSPENDED, &suspended))) data["suspended"] = suspended != 0;
        }
        const int end = offset < count ? offset + std::min(limit, count - offset) : count;
        // Build the GUID map once per page; never scan all loaded playlists per queue item.
        std::map<std::string, int> playlistIndexes;
        if (offset < end) for (int i = 0; i < std::min(manager->GetLoadedPlaylistCount(), kMaxPlaylists); ++i) {
            Ref<IAIMPPlaylist> candidate;
            if (SUCCEEDED(manager->GetLoadedPlaylist(i, candidate.put())) && candidate) {
                const auto guid = PlaylistGuid(candidate.p);
                if (!guid.empty()) playlistIndexes[guid] = i;
            }
        }
        for (int i = offset; i < end; ++i) {
            Ref<IAIMPPlaylistItem> item;
            if (!Check(queue->GetItem(i, IID_IAIMPPlaylistItem, item.out()), response, "Read queue item") || !item) return true;
            Ref<IAIMPPlaylist> playlist;
            int pidx = -1, tidx = -1;
            if (SUCCEEDED(item->GetValueAsObject(AIMP_PLAYLISTITEM_PROPID_PLAYLIST, IID_IAIMPPlaylist, playlist.out())) && playlist) {
                const auto found = playlistIndexes.find(PlaylistGuid(playlist.p));
                if (found != playlistIndexes.end()) pidx = found->second;
            }
            item->GetValueAsInt32(AIMP_PLAYLISTITEM_PROPID_INDEX, &tidx);
            auto entry = TrackInfo(item.p, pidx, tidx, false);
            entry["queue_index"] = i;
            data["items"].push_back(std::move(entry));
        }
        data["has_more"] = end < count;
        response = {{"queue", std::move(data)}}; return true;
    }
    if (action == "queue/suspended") {
        bool value = false;
        if (!Boolean(cmd, "value", value)) { response = Error("INVALID_VALUE"); return true; }
        Ref<IAIMPPropertyList> props;
        if (!Properties(queue.p, props, response)) return true;
        if (Check(props->SetValueAsInt32(AIMP_PLAYLISTQUEUE_PROPID_SUSPENDED, value ? 1 : 0), response, "Set queue suspension")) {
            int actual = 0;
            if (Check(props->GetValueAsInt32(AIMP_PLAYLISTQUEUE_PROPID_SUSPENDED, &actual), response, "Read queue suspension"))
                response = {{"ok", true}, {"queue", {{"count", queue->GetItemCount()}, {"suspended", actual != 0}}}};
        }
        return true;
    }
    if (action == "queue/clear") {
        if (count > kMaxQueueClear) { response = Error("QUEUE_TOO_LARGE", "Queue clearing is limited to 10000 items per operation"); return true; }
        int removed = 0;
        for (int i = 0; i < count; ++i) {
            const int before = queue->GetItemCount();
            if (before == 0) break;
            Ref<IAIMPPlaylistItem> item;
            if (!Check(queue->GetItem(0, IID_IAIMPPlaylistItem, item.out()), response, "Read queue item to remove") || !item ||
                !Check(queue->Delete(item.p), response, "Remove queue item")) {
                response["removed"] = removed; return true;
            }
            const int after = queue->GetItemCount();
            if (after >= before) { response = Error("QUEUE_DID_NOT_CHANGE"); response["removed"] = removed; return true; }
            removed += before - after;
        }
        response = {{"ok", true}, {"queue", {{"count", queue->GetItemCount()}}}, {"removed", removed}}; return true;
    }
    if (action == "queue/move") {
        int index = -1, target = -1;
        if (!Integer(cmd, "queue_index", index, 0, count - 1) || !Integer(cmd, "target_index", target, 0, count - 1)) {
            response = Error("INVALID_QUEUE_INDEX"); return true;
        }
        if (Check(queue->Move2(index, target), response, "Move queue item")) response = {{"ok", true}, {"queue", {{"count", queue->GetItemCount()}}}};
        return true;
    }
    Ref<IAIMPPlaylist> playlist;
    Ref<IAIMPPlaylistItem> item;
    int pidx = -1, tidx = -1;
    if (action == "queue/remove" && cmd.contains("queue_index")) {
        int index = -1;
        if (!Integer(cmd, "queue_index", index, 0, count - 1)) { response = Error("INVALID_QUEUE_INDEX"); return true; }
        if (!Check(queue->GetItem(index, IID_IAIMPPlaylistItem, item.out()), response, "Read queue item") || !item) return true;
    } else if (!ResolveTrack(manager.p, cmd, playlist, item, pidx, tidx, response)) return true;
    HRESULT hr = E_INVALIDARG;
    if (action == "queue/add") {
        bool first = false;
        if (!Boolean(cmd, "first", first, false)) { response = Error("INVALID_VALUE"); return true; }
        hr = queue->Add(item.p, first ? TRUE : FALSE);
    } else hr = queue->Delete(item.p);
    if (Check(hr, response, "Update queue")) response = {{"ok", true}, {"queue", {{"count", queue->GetItemCount()}}}};
    return true;
}
// The official C++ header incorrectly declares ItemsGet(..., IAIMPString*, ...).
// Delphi apiPlaylists.pas declares "out Path: IAIMPString". That ABI requires **.
// Keep the same IID, inheritance and five-method order; use the corrected slot.
class PlaylistFoldersABI : public IAIMPPlaylistPreimage {
public:
    virtual HRESULT WINAPI ItemsAdd(IAIMPString* path, BOOL recursive) = 0;
    virtual HRESULT WINAPI ItemsDelete(int index) = 0;
    virtual HRESULT WINAPI ItemsDeleteAll() = 0;
    virtual HRESULT WINAPI ItemsGet(int index, IAIMPString** path, BOOL* recursive) = 0;
    virtual int WINAPI ItemsGetCount() = 0;
};

enum class PropertyKind { Boolean, Text, Index };
struct PlaylistProperty { const char* id; const char* label; int property; PropertyKind kind; };
const PlaylistProperty playlistProperties[] = {
    {"name", "Playlist name", AIMP_PLAYLIST_PROPID_NAME, PropertyKind::Text},
    {"group_name", "Playlist group name", AIMP_PLAYLIST_PROPID_GROUPNAME, PropertyKind::Text},
    {"read_only", "Read-only playlist", AIMP_PLAYLIST_PROPID_READONLY, PropertyKind::Boolean},
    {"grouping_override", "Override grouping", AIMP_PLAYLIST_PROPID_GROUPPING_OVERRIDEN, PropertyKind::Boolean},
    {"grouping", "Group tracks", AIMP_PLAYLIST_PROPID_GROUPPING, PropertyKind::Boolean},
    {"grouping_template", "Grouping template", AIMP_PLAYLIST_PROPID_GROUPPING_TEMPLATE, PropertyKind::Text},
    {"grouping_auto_merge", "Automatically merge groups", AIMP_PLAYLIST_PROPID_GROUPPING_AUTOMERGING, PropertyKind::Boolean},
    {"format_override", "Override track formatting", AIMP_PLAYLIST_PROPID_FORMATING_OVERRIDEN, PropertyKind::Boolean},
    {"format_line1", "Track first-line template", AIMP_PLAYLIST_PROPID_FORMATING_LINE1_TEMPLATE, PropertyKind::Text},
    {"format_line2", "Track second-line template", AIMP_PLAYLIST_PROPID_FORMATING_LINE2_TEMPLATE, PropertyKind::Text},
    {"view_override", "Override playlist view", AIMP_PLAYLIST_PROPID_VIEW_OVERRIDEN, PropertyKind::Boolean},
    {"view_duration", "Show durations", AIMP_PLAYLIST_PROPID_VIEW_DURATION, PropertyKind::Boolean},
    {"view_expand_buttons", "Show group expand buttons", AIMP_PLAYLIST_PROPID_VIEW_EXPAND_BUTTONS, PropertyKind::Boolean},
    {"view_marks", "Show ratings", AIMP_PLAYLIST_PROPID_VIEW_MARKS, PropertyKind::Boolean},
    {"view_numbers", "Show track numbers", AIMP_PLAYLIST_PROPID_VIEW_NUMBERS, PropertyKind::Boolean},
    {"view_numbers_absolute", "Use absolute track numbers", AIMP_PLAYLIST_PROPID_VIEW_NUMBERS_ABSOLUTE, PropertyKind::Boolean},
    {"view_second_line", "Show second line", AIMP_PLAYLIST_PROPID_VIEW_SECOND_LINE, PropertyKind::Boolean},
    {"view_switches", "Show playback switches", AIMP_PLAYLIST_PROPID_VIEW_SWITCHES, PropertyKind::Boolean},
    {"focused_index", "Focused track index", AIMP_PLAYLIST_PROPID_FOCUSINDEX, PropertyKind::Index},
    {"playback_cursor", "Playback cursor", AIMP_PLAYLIST_PROPID_PLAYBACKCURSOR, PropertyKind::Index},
    {"playing_index", "Playing track index", AIMP_PLAYLIST_PROPID_PLAYINGINDEX, PropertyKind::Index}
};
const std::set<std::string> managementCommands = {
    "playlists/import", "playlists/track/move-to", "playlists/track/copy-to", "playlists/track/selected", "playlists/track/refresh",
    "playlists/selection/set", "playlists/groups/get", "playlists/group/set", "playlists/groups/merge",
    "playlists/properties/get", "playlists/properties/set", "playlists/sort/template", "playlists/refresh",
    "playlists/source/get", "playlists/source/create", "playlists/source/set", "playlists/source/sync"
};
bool BoundedString(const json& value, size_t limit, std::string& text, bool emptyAllowed = true) {
    if (!value.is_string()) return false;
    text = value.get<std::string>();
    return text.size() <= limit && text.find('\0') == std::string::npos && (emptyAllowed || !text.empty());
}
json PropertyDescriptors() {
    json result = json::array();
    for (const auto& p : playlistProperties) {
        json d = {{"id", p.id}, {"label", p.label}, {"type", p.kind == PropertyKind::Boolean ? "boolean" : p.kind == PropertyKind::Index ? "integer" : "string"}};
        if (p.kind == PropertyKind::Index) d["min"] = -1;
        if (p.kind == PropertyKind::Text) d["max_length"] = 4096;
        result.push_back(std::move(d));
    }
    return result;
}
json PropertyState(IAIMPPlaylist* playlist, int index) {
    json result = {{"playlist_id", index}, {"values", json::object()}, {"available", json::object()}};
    Ref<IAIMPPropertyList> props;
    if (FAILED(playlist->QueryInterface(IID_IAIMPPropertyList, props.out())) || !props) return result;
    for (const auto& property : playlistProperties) {
        if (property.kind == PropertyKind::Text) {
            Ref<IAIMPString> value;
            const bool available = SUCCEEDED(props->GetValueAsObject(property.property, IID_IAIMPString, value.out())) && value;
            result["available"][property.id] = available;
            if (available) result["values"][property.id] = ReadText(props.p, property.property, 4096);
        } else {
            int value = 0;
            const bool available = SUCCEEDED(props->GetValueAsInt32(property.property, &value));
            result["available"][property.id] = available;
            if (available) result["values"][property.id] = property.kind == PropertyKind::Boolean ? json(value != 0) : json(value);
        }
    }
    return result;
}
json SourceTypes() {
    Ref<IAIMPServicePlaylistManager2> manager;
    if (!Service(IID_IAIMPServicePlaylistManager2, manager)) return json::array();
    json result = json::array();
    struct Kind { const char* id; const char* label; const WCHAR* factory; };
    for (const auto& kind : {Kind{"folders", "Folders", AIMP_PREIMAGEFACTORY_FOLDERS_ID}, Kind{"playlist", "Playlist file or URL", AIMP_PREIMAGEFACTORY_PLAYLIST_ID}}) {
        Ref<IAIMPString> id; json failure;
        Ref<IAIMPExtensionPlaylistPreimageFactory> factory;
        if (MakeString(WStr(kind.factory), id, failure) && SUCCEEDED(manager->GetPreimageFactoryByID(id.p, factory.put())) && factory)
            result.push_back({{"id", kind.id}, {"label", kind.label}});
    }
    return result;
}
bool GetSource(IAIMPPlaylist* playlist, Ref<IAIMPPlaylistPreimage>& source, json& response) {
    Ref<IAIMPPropertyList> props;
    if (!Properties(playlist, props, response)) return false;
    if (FAILED(props->GetValueAsObject(AIMP_PLAYLIST_PROPID_PREIMAGE, IID_IAIMPPlaylistPreimage, source.out())) || !source) {
        response = Error("NO_PLAYLIST_SOURCE"); return false;
    }
    return true;
}
json SourceState(IAIMPPlaylistPreimage* source, int index) {
    json result = {{"playlist_id", index}, {"attached", source != nullptr}, {"available", json::object()}};
    if (!source) return result;
    const auto factory = ReadText(source, AIMP_PLAYLISTPREIMAGE_PROPID_FACTORYID);
    result["factory_id"] = factory;
    result["sort_template"] = ReadText(source, AIMP_PLAYLISTPREIMAGE_PROPID_SORTTEMPLATE, 4096);
    for (const auto& field : {std::pair<const char*, int>{"autosync", AIMP_PLAYLISTPREIMAGE_PROPID_AUTOSYNC}, {"autosync_on_startup", AIMP_PLAYLISTPREIMAGE_PROPID_AUTOSYNC_ON_STARTUP}}) {
        int value = 0;
        const bool available = SUCCEEDED(source->GetValueAsInt32(field.second, &value));
        result["available"][field.first] = available;
        if (available) result[field.first] = value != 0;
    }
    Ref<PlaylistFoldersABI> folders;
    if (SUCCEEDED(source->QueryInterface(IID_IAIMPPlaylistPreimageFolders, folders.out())) && folders) {
        const int count = folders->ItemsGetCount();
        if (count >= 0 && count <= 256) {
            result["folders"] = json::array();
            for (int i = 0; i < count; ++i) {
                Ref<IAIMPString> path; BOOL recursive = FALSE;
                if (FAILED(folders->ItemsGet(i, path.put(), &recursive)) || !path) { result["read_error"] = "SOURCE_FOLDER_READ_FAILED"; break; }
                const auto* data = path->GetData();
                const int size = path->GetLength();
                if (data && size >= 0 && size <= 16384) result["folders"].push_back({{"path", WStrToUtf8(std::wstring(data, size))}, {"recursive", recursive != FALSE}});
                else { result["read_error"] = "SOURCE_PATH_TOO_LONG"; break; }
            }
        } else result["read_error"] = "SOURCE_FOLDER_LIMIT";
    } else if (factory == WStr(AIMP_PREIMAGEFACTORY_PLAYLIST_ID)) {
        result["uri"] = ReadText(source, AIMP_PLAYLISTPREIMAGE_PLAYLISTBASED_PROPID_URI, 16384);
    }
    return result;
}
struct PlaylistUpdate {
    IAIMPPlaylist* playlist;
    bool active = false;
    explicit PlaylistUpdate(IAIMPPlaylist* p) : playlist(p) {}
    ~PlaylistUpdate() { if (active) playlist->EndUpdate(); }
    HRESULT Begin() { const auto hr = playlist->BeginUpdate(); active = SUCCEEDED(hr); return hr; }
    HRESULT End() { if (!active) return S_OK; active = false; return playlist->EndUpdate(); }
};

bool HandleManagement(const std::string& action, const json& cmd, json& response) {
    if (!managementCommands.count(action)) return false;
    if (!g_core || GetCurrentThreadId() != g_mainThreadId) { response = Error("AIMP_MAIN_THREAD_REQUIRED"); return true; }
    Ref<IAIMPServicePlaylistManager> manager;
    if (!Manager(manager, response)) return true;
    Ref<IAIMPPlaylist> playlist;
    int index = -1;
    if (action == "playlists/import") {
        std::string uri; bool activate = true;
        if (!Text(cmd, "uri", uri, 16384) || !Boolean(cmd, "activate", activate, false)) { response = Error("INVALID_PARAMETER"); return true; }
        if (manager->GetLoadedPlaylistCount() >= kMaxPlaylists) { response = Error("TOO_MANY_PLAYLISTS"); return true; }
        Ref<IAIMPString> text;
        if (!MakeString(uri, text, response)) return true;
        if (Check(manager->CreatePlaylistFromFile(text.p, activate ? TRUE : FALSE, playlist.put()), response, "Import playlist") && playlist)
            response = {{"ok", true}, {"accepted", true}, {"library_changed", true}, {"playlist", PlaylistInfo(playlist.p, PlaylistIndex(manager.p, playlist.p))}};
        return true;
    }
    if (action.compare(0, 16, "playlists/track/") == 0) {
        Ref<IAIMPPlaylistItem> item; int trackIndex = -1;
        if (!ResolveTrack(manager.p, cmd, playlist, item, index, trackIndex, response)) return true;
        HRESULT result = E_INVALIDARG;
        if (action == "playlists/track/selected") {
            bool value = false;
            if (!Boolean(cmd, "value", value)) { response = Error("INVALID_VALUE"); return true; }
            result = item->SetValueAsInt32(AIMP_PLAYLISTITEM_PROPID_SELECTED, value ? 1 : 0);
        } else if (action == "playlists/track/refresh") result = item->ReloadInfo();
        else {
            int destination = -1;
            if (!Integer(cmd, "destination_playlist_id", destination, 0, manager->GetLoadedPlaylistCount() - 1)) { response = Error("INVALID_DESTINATION_PLAYLIST"); return true; }
            Ref<IAIMPPlaylist> target;
            if (!Check(manager->GetLoadedPlaylist(destination, target.put()), response, "Read destination playlist") || !target) return true;
            if (action == "playlists/track/move-to") result = item->SetValueAsObject(AIMP_PLAYLISTITEM_PROPID_PLAYLIST, target.p);
            else {
                int insert = -1;
                if (!Integer(cmd, "insert_at", insert, -1, target->GetItemCount(), false)) { response = Error("INVALID_INSERT_POSITION"); return true; }
                Ref<IAIMPString> file;
                if (!Check(item->GetValueAsObject(AIMP_PLAYLISTITEM_PROPID_FILENAME, IID_IAIMPString, file.out()), response, "Read source track URI") || !file) return true;
                result = target->Add(file.p, AIMP_PLAYLIST_ADD_FLAGS_NOEXPAND, insert);
            }
        }
        if (Check(result, response, "Update playlist track")) {
            response = {{"ok", true}, {"library_changed", true}};
            if (action == "playlists/track/copy-to") response["accepted"] = true;
        }
        return true;
    }
    if (!ResolvePlaylist(manager.p, cmd, playlist, index, response)) return true;
    if (action == "playlists/selection/set") {
        std::string mode;
        if (!Text(cmd, "mode", mode, 16) || (mode != "all" && mode != "none" && mode != "invert")) { response = Error("INVALID_SELECTION_MODE"); return true; }
        const int count = playlist->GetItemCount();
        if (count < 0 || count > 100000) { response = Error("PLAYLIST_OPERATION_LIMIT"); return true; }
        PlaylistUpdate update(playlist.p);
        if (!Check(update.Begin(), response, "Begin playlist selection")) return true;
        int changed = 0;
        for (int i = 0; i < count; ++i) {
            Ref<IAIMPPlaylistItem> item;
            if (!Check(playlist->GetItem(i, IID_IAIMPPlaylistItem, item.out()), response, "Read selected item") || !item) break;
            int value = 0;
            if (mode == "invert" && !Check(item->GetValueAsInt32(AIMP_PLAYLISTITEM_PROPID_SELECTED, &value), response, "Read track selection")) break;
            const bool selected = mode == "all" || (mode == "invert" && value == 0);
            if (!Check(item->SetValueAsInt32(AIMP_PLAYLISTITEM_PROPID_SELECTED, selected ? 1 : 0), response, "Set track selection")) break;
            ++changed;
        }
        const auto ended = update.End();
        if (!response.contains("error")) Check(ended, response, "End playlist selection");
        if (!response.contains("error") && changed == count) response = {{"ok", true}};
        else if (!response.contains("error")) response = Error("SELECTION_INCOMPLETE");
        response["changed"] = changed; response["library_changed"] = changed > 0;
        return true;
    }
    if (action == "playlists/groups/get") {
        int offset = 0, limit = 100; const int total = playlist->GetGroupCount();
        if (total < 0 || !Integer(cmd, "offset", offset, 0, INT_MAX, false) || !Integer(cmd, "limit", limit, 1, 250, false)) { response = Error("INVALID_GROUP_PAGE"); return true; }
        json groups = {{"playlist_id", index}, {"total", total}, {"offset", offset}, {"items", json::array()}};
        const int end = offset < total ? offset + std::min(limit, total - offset) : total;
        for (int i = offset; i < end; ++i) {
            Ref<IAIMPPlaylistGroup> group;
            if (!Check(playlist->GetGroup(i, IID_IAIMPPlaylistGroup, group.out()), response, "Read playlist group") || !group) return true;
            json entry = {{"id", i}, {"name", ReadText(group.p, AIMP_PLAYLISTGROUP_PROPID_NAME, 4096)}, {"track_count", group->GetItemCount()}};
            int value = 0; double seconds = 0;
            if (SUCCEEDED(group->GetValueAsInt32(AIMP_PLAYLISTGROUP_PROPID_EXPANDED, &value))) entry["expanded"] = value != 0;
            if (SUCCEEDED(group->GetValueAsInt32(AIMP_PLAYLISTGROUP_PROPID_SELECTED, &value))) entry["selected"] = value != 0;
            if (SUCCEEDED(group->GetValueAsFloat(AIMP_PLAYLISTGROUP_PROPID_DURATION, &seconds)) && std::isfinite(seconds)) entry["duration"] = seconds;
            groups["items"].push_back(std::move(entry));
        }
        groups["has_more"] = end < total; response = {{"groups", std::move(groups)}}; return true;
    }
    if (action == "playlists/group/set" || action == "playlists/groups/merge") {
        Ref<IAIMPPlaylistGroup> group;
        if (action == "playlists/group/set" || cmd.contains("group_index")) {
            int g = -1;
            if (!Integer(cmd, "group_index", g, 0, playlist->GetGroupCount() - 1)) { response = Error("INVALID_GROUP_INDEX"); return true; }
            if (!Check(playlist->GetGroup(g, IID_IAIMPPlaylistGroup, group.out()), response, "Read playlist group") || !group) return true;
        }
        HRESULT result;
        if (action == "playlists/groups/merge") result = playlist->MergeGroup(group.p);
        else {
            std::string parameter; bool value = false;
            if (!Text(cmd, "parameter", parameter, 32) || !Boolean(cmd, "value", value) || (parameter != "expanded" && parameter != "selected")) { response = Error("INVALID_PARAMETER"); return true; }
            result = group->SetValueAsInt32(parameter == "expanded" ? AIMP_PLAYLISTGROUP_PROPID_EXPANDED : AIMP_PLAYLISTGROUP_PROPID_SELECTED, value ? 1 : 0);
        }
        if (Check(result, response, "Update playlist group")) response = {{"ok", true}, {"library_changed", true}};
        return true;
    }
    if (action == "playlists/properties/get") { response = {{"playlist_properties", PropertyState(playlist.p, index)}}; return true; }
    if (action == "playlists/properties/set") {
        std::string parameter;
        if (!Text(cmd, "parameter", parameter, 64) || !cmd.contains("value")) { response = Error("INVALID_PARAMETER"); return true; }
        const PlaylistProperty* descriptor = nullptr;
        for (const auto& p : playlistProperties) if (parameter == p.id) descriptor = &p;
        if (!descriptor) { response = Error("UNSUPPORTED_PLAYLIST_PROPERTY"); return true; }
        Ref<IAIMPPropertyList> props;
        if (!Properties(playlist.p, props, response)) return true;
        HRESULT result = E_INVALIDARG;
        if (descriptor->kind == PropertyKind::Text) {
            std::string value;
            if (!BoundedString(cmd["value"], 4096, value, parameter != "name")) { response = Error("INVALID_VALUE"); return true; }
            Ref<IAIMPString> text;
            if (!MakeString(value, text, response)) return true;
            result = props->SetValueAsObject(descriptor->property, text.p);
        } else if (descriptor->kind == PropertyKind::Boolean) {
            bool value = false;
            if (!Boolean(cmd, "value", value)) { response = Error("INVALID_VALUE"); return true; }
            result = props->SetValueAsInt32(descriptor->property, value ? 1 : 0);
        } else {
            int value = -1;
            if (!Integer(cmd, "value", value, -1, playlist->GetItemCount() - 1)) { response = Error("INVALID_TRACK_ID"); return true; }
            result = props->SetValueAsInt32(descriptor->property, value);
        }
        if (Check(result, response, "Set playlist property")) response = {{"ok", true}, {"library_changed", true}, {"playlist_properties", PropertyState(playlist.p, index)}};
        return true;
    }
    if (action == "playlists/sort/template") {
        std::string format;
        if (!Text(cmd, "template", format, 4096)) { response = Error("INVALID_SORT_TEMPLATE"); return true; }
        Ref<IAIMPString> text;
        if (!MakeString(format, text, response)) return true;
        if (Check(playlist->Sort2(text.p), response, "Sort playlist by template")) response = {{"ok", true}, {"library_changed", true}};
        return true;
    }
    if (action == "playlists/refresh") {
        if (cmd.contains("mode") && !cmd["mode"].is_string()) { response = Error("INVALID_REFRESH_MODE"); return true; }
        const std::string mode = cmd.value("mode", "missing");
        if (mode != "missing" && mode != "all" && mode != "selected") { response = Error("INVALID_REFRESH_MODE"); return true; }
        const int flags = mode == "all" ? AIMP_PLAYLIST_RELOADINFO_FLAGS_FULL : mode == "selected" ? AIMP_PLAYLIST_RELOADINFO_FLAGS_SELECTED : AIMP_PLAYLIST_RELOADINFO_FLAGS_DEFAULT;
        if (Check(playlist->ReloadInfo(flags), response, "Refresh playlist metadata")) response = {{"ok", true}, {"accepted", true}, {"library_changed", true}};
        return true;
    }
    if (action == "playlists/source/sync") {
        if (Check(playlist->ReloadFromPreimage(), response, "Synchronize playlist source")) response = {{"ok", true}, {"accepted", true}, {"library_changed", true}};
        return true;
    }
    Ref<IAIMPPlaylistPreimage> source;
    if (action == "playlists/source/create") {
        std::string kind;
        if (!Text(cmd, "kind", kind, 32) || (kind != "folders" && kind != "playlist")) { response = Error("INVALID_SOURCE_TYPE"); return true; }
        if (kind == "folders") {
            if (!cmd.contains("folders") || !cmd["folders"].is_array() || cmd["folders"].empty() || cmd["folders"].size() > 256) { response = Error("INVALID_SOURCE_FOLDERS"); return true; }
            for (const auto& folder : cmd["folders"]) {
                std::string path; bool recursive = false;
                if (!folder.is_object() || !Text(folder, "path", path, 16384) || !Boolean(folder, "recursive", recursive, false)) { response = Error("INVALID_SOURCE_FOLDER"); return true; }
            }
        } else { std::string uri; if (!Text(cmd, "uri", uri, 16384)) { response = Error("INVALID_MEDIA_URI"); return true; } }
        // Validate every optional field before changing any playlist/source setting.
        for (const char* key : {"autosync", "autosync_on_startup"}) if (cmd.contains(key) && !cmd[key].is_boolean()) { response = Error("INVALID_VALUE"); return true; }
        std::string sortTemplate;
        if (cmd.contains("sort_template") && !BoundedString(cmd["sort_template"], 4096, sortTemplate)) { response = Error("INVALID_VALUE"); return true; }
        Ref<IAIMPServicePlaylistManager2> manager2;
        if (!Service(IID_IAIMPServicePlaylistManager2, manager2)) { response = Error("PLAYLIST_SOURCE_UNAVAILABLE"); return true; }
        Ref<IAIMPString> factoryId;
        if (!MakeString(WStr(kind == "folders" ? AIMP_PREIMAGEFACTORY_FOLDERS_ID : AIMP_PREIMAGEFACTORY_PLAYLIST_ID), factoryId, response)) return true;
        Ref<IAIMPExtensionPlaylistPreimageFactory> factory;
        if (!Check(manager2->GetPreimageFactoryByID(factoryId.p, factory.put()), response, "Find playlist source factory") || !factory ||
            !Check(factory->CreatePreimage(source.put()), response, "Create playlist source") || !source) return true;
        if (kind == "folders") {
            Ref<PlaylistFoldersABI> folders;
            if (!Check(source->QueryInterface(IID_IAIMPPlaylistPreimageFolders, folders.out()), response, "Access folder source") || !folders) return true;
            for (const auto& folder : cmd["folders"]) {
                Ref<IAIMPString> path;
                if (!MakeString(folder["path"].get<std::string>(), path, response) || !Check(folders->ItemsAdd(path.p, folder.value("recursive", false) ? TRUE : FALSE), response, "Add source folder")) return true;
            }
        } else {
            Ref<IAIMPString> uri;
            if (!MakeString(cmd["uri"].get<std::string>(), uri, response) || !Check(source->SetValueAsObject(AIMP_PLAYLISTPREIMAGE_PLAYLISTBASED_PROPID_URI, uri.p), response, "Set playlist source URI")) return true;
        }
        for (const auto& setting : {std::pair<const char*, int>{"autosync", AIMP_PLAYLISTPREIMAGE_PROPID_AUTOSYNC}, {"autosync_on_startup", AIMP_PLAYLISTPREIMAGE_PROPID_AUTOSYNC_ON_STARTUP}})
            if (cmd.contains(setting.first) && !Check(source->SetValueAsInt32(setting.second, cmd[setting.first].get<bool>() ? 1 : 0), response, "Set source synchronization")) return true;
        if (cmd.contains("sort_template")) {
            Ref<IAIMPString> text;
            if (!MakeString(sortTemplate, text, response) || !Check(source->SetValueAsObject(AIMP_PLAYLISTPREIMAGE_PROPID_SORTTEMPLATE, text.p), response, "Set source sorting")) return true;
        }
        Ref<IAIMPPropertyList> props;
        if (!Properties(playlist.p, props, response) || !Check(props->SetValueAsObject(AIMP_PLAYLIST_PROPID_PREIMAGE, source.p), response, "Attach playlist source")) return true;
        response = {{"ok", true}, {"accepted", true}, {"library_changed", true}, {"playlist_source", SourceState(source.p, index)}};
        return true;
    }
    if (!GetSource(playlist.p, source, response)) {
        if (action == "playlists/source/get" && response["error"]["code"] == "NO_PLAYLIST_SOURCE") response = {{"playlist_source", SourceState(nullptr, index)}};
        return true;
    }
    if (action == "playlists/source/get") { response = {{"playlist_source", SourceState(source.p, index)}}; return true; }
    if (action == "playlists/source/set") {
        std::string parameter;
        if (!Text(cmd, "parameter", parameter, 32) || !cmd.contains("value")) { response = Error("INVALID_PARAMETER"); return true; }
        HRESULT result = E_INVALIDARG;
        if (parameter == "autosync" || parameter == "autosync_on_startup") {
            bool value = false;
            if (!Boolean(cmd, "value", value)) { response = Error("INVALID_VALUE"); return true; }
            result = source->SetValueAsInt32(parameter == "autosync" ? AIMP_PLAYLISTPREIMAGE_PROPID_AUTOSYNC : AIMP_PLAYLISTPREIMAGE_PROPID_AUTOSYNC_ON_STARTUP, value ? 1 : 0);
        } else if (parameter == "sort_template") {
            std::string value;
            if (!BoundedString(cmd["value"], 4096, value)) { response = Error("INVALID_VALUE"); return true; }
            Ref<IAIMPString> text;
            if (!MakeString(value, text, response)) return true;
            result = source->SetValueAsObject(AIMP_PLAYLISTPREIMAGE_PROPID_SORTTEMPLATE, text.p);
        } else { response = Error("UNSUPPORTED_SOURCE_PROPERTY"); return true; }
        if (Check(result, response, "Update playlist source")) response = {{"ok", true}, {"library_changed", true}, {"playlist_source", SourceState(source.p, index)}};
        return true;
    }
    response = Error("UNKNOWN_ACTION"); return true;
}

}

json GetLibraryCapabilities() {
    Ref<IAIMPServicePlaylistManager> manager;
    Ref<IAIMPPlaylistQueue> queue;
    Ref<IAIMPServicePlayer> player;
    const bool available = Service(IID_IAIMPServicePlaylistManager, manager);
    const bool queueAvailable = available && SUCCEEDED(manager->QueryInterface(IID_IAIMPPlaylistQueue, queue.out())) && queue;
    const json sourceTypes = available ? SourceTypes() : json::array();
    Ref<IAIMPPropertyList> props;
    int suspended = 0;
    const bool canSuspend = queueAvailable && SUCCEEDED(queue->QueryInterface(IID_IAIMPPropertyList, props.out())) && props &&
        SUCCEEDED(props->GetValueAsInt32(AIMP_PLAYLISTQUEUE_PROPID_SUSPENDED, &suspended));
    json result = {{"available", available}, {"queue", queueAvailable}, {"queue_suspended", canSuspend}, {"commands", json::array()},
        {"track_targets", {"focused", "playing", "explicit"}}, {"playlist_targets", {"active", "focused", "playing", "explicit"}},
        {"rating", {{"min", 0}, {"max", 5}, {"step", 0.5}}},
        {"limits", {{"queue_page", kMaxQueuePage}, {"queue_clear", kMaxQueueClear}, {"metadata_text", 4096}}}, {"sort_modes", json::array()}};
    if (available) {
        for (const auto& action : playlistCommands) result["commands"].push_back(action);
        for (const auto& action : trackCommands) result["commands"].push_back(action);
        for (const auto& action : managementCommands)
            if (action != "playlists/source/create" || !sourceTypes.empty()) result["commands"].push_back(action);
        for (const auto& mode : sorts) result["sort_modes"].push_back({{"id", mode.id}, {"label", mode.label}});
    }
    if (queueAvailable) for (const auto& action : queueCommands)
        if (action != "queue/suspended" || canSuspend) result["commands"].push_back(action);
    if (Service(IID_IAIMPServicePlayer, player)) {
        result["commands"].push_back("player/fileinfo");
        result["commands"].push_back("player/play-file");
    }
    result["playlist_properties"] = PropertyDescriptors();
    result["source_types"] = sourceTypes;
    return result;
}

bool HandleLibraryCommand(const std::string& action, const json& cmd, json& response) {
    if (HandleManagement(action, cmd, response)) return true;
    if (!playlistCommands.count(action) && !trackCommands.count(action) && !queueCommands.count(action) &&
        action != "player/fileinfo" && action != "player/play-file") return false;
    if (!g_core || GetCurrentThreadId() != g_mainThreadId) { response = Error("AIMP_MAIN_THREAD_REQUIRED"); return true; }
    if (action == "player/play-file") {
        std::string uri;
        if (!Text(cmd, "uri", uri, 16384) || std::any_of(uri.begin(), uri.end(), [](unsigned char c) { return c < 32 || c == 127; })) {
            response = Error("INVALID_MEDIA_URI"); return true;
        }
        Ref<IAIMPServicePlayer> player;
        Ref<IAIMPString> text;
        if (!Service(IID_IAIMPServicePlayer, player)) { response = Error("PLAYER_UNAVAILABLE"); return true; }
        if (!MakeString(uri, text, response)) return true;
        // SDK Play4 accepts a FileURI directly and asynchronously loads media.
        // It never invokes a shell. A normal media file does not enter a playlist;
        // AIMP necessarily imports playlist-file URIs even with this flag.
        if (Check(player->Play4(text.p, AIMP_SERVICE_PLAYER_FLAGS_PLAY_WITHOUT_ADDING_TO_PLAYLIST), response, "Request media playback"))
            response = {{"ok", true}, {"accepted", true}, {"library_changed", true}};
        return true;
    }
    if (action == "player/fileinfo") {
        Ref<IAIMPServicePlayer> player;
        Ref<IAIMPFileInfo> info;
        if (!Service(IID_IAIMPServicePlayer, player)) { response = Error("PLAYER_UNAVAILABLE"); return true; }
        const HRESULT result = player->GetInfo(info.put());
        if (FAILED(result) || !info) {
            if (player->GetState() == AIMP_PLAYER_STATE_STOPPED) response = {{"file_info", nullptr}};
            else if (Check(result, response, "Read playing file information")) response = Error("FILE_INFO_UNAVAILABLE");
            return true;
        }
        json metadata = json::object(); ReadInfo(info.p, metadata, true);
        response = {{"file_info", std::move(metadata)}}; return true;
    }
    if (queueCommands.count(action)) return QueueCommand(action, cmd, response);
    Ref<IAIMPServicePlaylistManager> manager;
    if (!Manager(manager, response)) return true;
    Ref<IAIMPPlaylist> playlist;
    int index = -1;
    if (action == "playlists/create") {
        std::string name;
        bool activate = true;
        if (!Text(cmd, "name", name, 1024) || !Boolean(cmd, "activate", activate, false)) { response = Error("INVALID_PARAMETER"); return true; }
        Ref<IAIMPString> text;
        if (!MakeString(name, text, response)) return true;
        if (manager->GetLoadedPlaylistCount() >= kMaxPlaylists) { response = Error("TOO_MANY_PLAYLISTS"); return true; }
        if (!Check(manager->CreatePlaylist(text.p, activate ? TRUE : FALSE, playlist.put()), response, "Create playlist") || !playlist) return true;
        response = {{"ok", true}, {"library_changed", true}, {"playlist", PlaylistInfo(playlist.p, PlaylistIndex(manager.p, playlist.p))}};
        return true;
    }
    if (trackCommands.count(action)) {
        Ref<IAIMPPlaylistItem> item;
        int trackIndex = -1;
        if (!ResolveTrack(manager.p, cmd, playlist, item, index, trackIndex, response)) return true;
        if (action == "playlists/track/info") { response = {{"track", TrackInfo(item.p, index, trackIndex, true)}}; return true; }
        HRESULT hr = E_INVALIDARG;
        if (action == "playlists/track/remove") hr = playlist->Delete(item.p);
        else if (action == "playlists/track/move") {
            int destination = -1;
            if (!Integer(cmd, "target_index", destination, 0, playlist->GetItemCount() - 1)) { response = Error("INVALID_TRACK_ID"); return true; }
            hr = item->SetValueAsInt32(AIMP_PLAYLISTITEM_PROPID_INDEX, destination);
        } else if (action == "playlists/track/enabled") {
            bool enabled = false;
            if (!Boolean(cmd, "value", enabled)) { response = Error("INVALID_VALUE"); return true; }
            hr = item->SetValueAsInt32(AIMP_PLAYLISTITEM_PROPID_PLAYINGSWITCH, enabled ? 1 : 0);
        } else if (action == "playlists/track/rating") {
            if (!cmd.contains("value") || !cmd["value"].is_number()) { response = Error("INVALID_RATING"); return true; }
            const double value = cmd["value"].get<double>();
            if (!std::isfinite(value) || value < 0 || value > 5) { response = Error("INVALID_RATING"); return true; }
            hr = item->SetValueAsFloat(AIMP_PLAYLISTITEM_PROPID_MARK, value);
        }
        if (Check(hr, response, "Update playlist track")) response = {{"ok", true}, {"library_changed", true}};
        return true;
    }
    if (!ResolvePlaylist(manager.p, cmd, playlist, index, response)) return true;
    if (action == "playlists/info") { response = {{"playlist", PlaylistInfo(playlist.p, index)}}; return true; }
    HRESULT hr = E_INVALIDARG;
    if (action == "playlists/rename") {
        std::string name;
        if (!Text(cmd, "name", name, 1024)) { response = Error("INVALID_NAME"); return true; }
        Ref<IAIMPString> text;
        Ref<IAIMPPropertyList> props;
        if (!MakeString(name, text, response) || !Properties(playlist.p, props, response)) return true;
        hr = props->SetValueAsObject(AIMP_PLAYLIST_PROPID_NAME, text.p);
    } else if (action == "playlists/close") {
        // FORCE_UNLOAD closes the loaded tab. Never FORCE_REMOVE, which also removes the saved playlist.
        hr = playlist->Close(AIMP_PLAYLIST_CLOSE_FLAGS_FORCE_UNLOAD);
    } else if (action == "playlists/clear") {
        // Playlist entry removal only. Never Delete3(PHYSICALLY), tag writers, or file-system delete APIs.
        hr = playlist->DeleteAll();
    } else if (action == "playlists/add") {
        std::string uri;
        int insertion = -1;
        if (!Text(cmd, "uri", uri, 16384) || !Integer(cmd, "insert_at", insertion, -1, playlist->GetItemCount(), false)) {
            response = Error("INVALID_MEDIA_URI"); return true;
        }
        Ref<IAIMPString> text;
        if (!MakeString(uri, text, response)) return true;
        // One media URI, no recursive folder/archive expansion, and keep normal AIMP decoder checks.
        // Let AIMP schedule file inspection; do not block its GUI thread on media I/O.
        hr = playlist->Add(text.p, AIMP_PLAYLIST_ADD_FLAGS_NOEXPAND, insertion);
    } else if (action == "playlists/sort") {
        std::string mode;
        if (!Text(cmd, "mode", mode, 32)) { response = Error("INVALID_SORT_MODE"); return true; }
        int sort = -1;
        for (const auto& candidate : sorts) if (mode == candidate.id) sort = candidate.value;
        if (sort < 0) { response = Error("INVALID_SORT_MODE"); return true; }
        hr = playlist->Sort(sort);
    }
    if (Check(hr, response, "Update playlist")) {
        response = {{"ok", true}, {"library_changed", true}};
        if (action == "playlists/add") response["accepted"] = true;
    }
    return true;
}

namespace {
bool ResolveTagFile(IAIMPServicePlaylistManager* manager, const json& cmd, Ref<IAIMPPlaylist>& playlist,
                    Ref<IAIMPPlaylistItem>& item, Ref<IAIMPString>& file, std::string& filename, json& response) {
    std::string target = (cmd.contains("playlist_id") || cmd.contains("track_id")) ? "explicit" : "focused";
    if (cmd.contains("target")) {
        if (!cmd["target"].is_string()) { response = Error("INVALID_TARGET"); return false; }
        target = cmd["target"].get<std::string>();
    }
    if (target == "playing") {
        Ref<IAIMPServicePlayer> player;
        Ref<IAIMPFileInfo> info;
        if (!Service(IID_IAIMPServicePlayer, player) || !Check(player->GetInfo(info.put()), response, "Read playing file") || !info) return false;
        if (!Check(info->GetValueAsObject(AIMP_FILEINFO_PROPID_FILENAME, IID_IAIMPString, file.out()), response, "Read playing file path") || !file) return false;
        // Optional playlist entry, used only to refresh metadata after a successful save.
        player->GetPlaylistItem(item.put());
    } else {
        int p = -1, t = -1;
        if (!ResolveTrack(manager, cmd, playlist, item, p, t, response)) return false;
        if (!Check(item->GetValueAsObject(AIMP_PLAYLISTITEM_PROPID_FILENAME, IID_IAIMPString, file.out()), response, "Read target file path") || !file) return false;
    }
    const int size = file->GetLength(); const auto* data = file->GetData();
    if (!data || size < 1 || size > 16384) { response = Error("INVALID_TAG_FILE_PATH"); return false; }
    filename = WStrToUtf8(std::wstring(data, size));
    if (filename.empty() || filename.size() > 16384 || filename.find('\0') != std::string::npos) { response = Error("INVALID_TAG_FILE_PATH"); return false; }
    // The tag writer operates on one existing media file, never a stream/URL or device.
    const bool drivePath = size >= 3 && ((data[0] >= L'A' && data[0] <= L'Z') || (data[0] >= L'a' && data[0] <= L'z')) && data[1] == L':' && (data[2] == L'\\' || data[2] == L'/');
    const bool uncPath = size >= 5 && data[0] == L'\\' && data[1] == L'\\' && data[2] != L'.' && data[2] != L'?' && data[2] != L'\\';
    if (!drivePath && !uncPath) { response = Error("TAG_LOCAL_FILE_REQUIRED", "Choose a local or network-share media file, not an internet or virtual stream"); return false; }
    return true;
}
json ReadTagFields(IAIMPFileInfo* info) {
    json fields = json::object();
    for (const auto& field : bridge_tags::Fields) {
        if (std::string(field.type) == "string") {
            Ref<IAIMPString> value;
            if (SUCCEEDED(info->GetValueAsObject(field.property, IID_IAIMPString, value.out())) && value) {
                const auto* data = value->GetData();
                size_t size = std::min(field.maxLength, static_cast<size_t>(std::max(0, value->GetLength())));
                if (data) {
                    if (size && data[size - 1] >= 0xd800 && data[size - 1] <= 0xdbff) --size;
                    fields[field.id] = WStrToUtf8(std::wstring(data, size));
                }
            }
        } else if (std::string(field.type) == "integer") {
            int value = 0; if (SUCCEEDED(info->GetValueAsInt32(field.property, &value))) fields[field.id] = value;
        } else {
            double value = 0; if (SUCCEEDED(info->GetValueAsFloat(field.property, &value)) && std::isfinite(value)) fields[field.id] = value;
        }
    }
    return fields;
}
}

json GetTagCapabilities() {
    Ref<IAIMPServiceFileTagEditor> service;
    const bool available = Service(IID_IAIMPServiceFileTagEditor, service);
    json fields = json::array();
    for (const auto& field : bridge_tags::Fields) {
        json descriptor = {{"id", field.id}, {"label", field.label}, {"type", field.type}};
        if (std::string(field.type) == "string") descriptor["max_length"] = field.maxLength;
        else { descriptor["min"] = field.minimum; descriptor["max"] = field.maximum; }
        fields.push_back(std::move(descriptor));
    }
    return {{"available", available}, {"requires_confirmation", true}, {"requires_file_identity", true},
            {"fields", std::move(fields)}, {"commands", available ? json({"tags/get", "tags/write"}) : json::array()},
            {"maximum_text_bytes", 49152}, {"targets", {"focused", "playing", "explicit"}}};
}

bool HandleTagCommand(const std::string& action, const json& cmd, json& response) {
    if (action != "tags/get" && action != "tags/write") return false;
    if (!g_core || GetCurrentThreadId() != g_mainThreadId) { response = Error("AIMP_MAIN_THREAD_REQUIRED"); return true; }
    if (action == "tags/write") {
        const auto error = bridge_tags::ValidateWrite(cmd);
        if (!error.empty()) { response = Error(error.c_str()); return true; }
    }
    Ref<IAIMPServiceFileTagEditor> service;
    if (!Service(IID_IAIMPServiceFileTagEditor, service)) { response = Error("TAG_EDITOR_UNAVAILABLE"); return true; }
    Ref<IAIMPServicePlaylistManager> manager;
    if (!Manager(manager, response)) return true;
    Ref<IAIMPPlaylist> playlist;
    Ref<IAIMPPlaylistItem> item;
    Ref<IAIMPString> file;
    std::string filename;
    if (!ResolveTagFile(manager.p, cmd, playlist, item, file, filename, response)) return true;
    if (action == "tags/write" && cmd["expected_file_path"].get<std::string>() != filename) {
        response = Error("TAG_FILE_CHANGED", "The selected file changed after it was read; no tags were written"); return true;
    }
    Ref<IAIMPFileTagEditor> editor;
    if (!Check(service->EditFile(file.p, IID_IAIMPFileTagEditor, editor.out()), response, "Open supported media tag editor") || !editor) return true;
    const int count = editor->GetTagCount();
    if (count < 0 || count > 256) { response = Error("INVALID_TAG_COUNT"); return true; }
    Ref<IAIMPFileInfo> mixed;
    if (!Check(editor->GetMixedInfo(mixed.put()), response, "Read existing file tags") || !mixed) return true;
    if (action == "tags/get") {
        response = {{"tags", {{"file_path", filename}, {"tag_count", count}, {"fields", ReadTagFields(mixed.p)}}}}; return true;
    }
    // Work only on a private copy of the editor's mixed tag information. Never
    // mutate the player's global file-info object. SetToAll retains unspecified
    // merged values, but can normalize differences between existing tag formats.
    Ref<IAIMPFileInfo> edited;
    if (!Check(mixed->Clone(edited.put()), response, "Clone existing tags") || !edited) return true;
    for (auto entry = cmd["fields"].begin(); entry != cmd["fields"].end(); ++entry) {
        const auto* field = bridge_tags::Find(entry.key()); // Validated before opening the file.
        HRESULT result;
        if (std::string(field->type) == "string") {
            Ref<IAIMPString> value;
            if (!MakeString(entry.value().get<std::string>(), value, response)) return true;
            result = edited->SetValueAsObject(field->property, value.p);
        } else if (std::string(field->type) == "integer") result = edited->SetValueAsInt32(field->property, entry.value().get<int>());
        else result = edited->SetValueAsFloat(field->property, entry.value().get<double>());
        if (!Check(result, response, "Set validated tag field")) { response["error"]["field"] = entry.key(); return true; }
    }
    if (!Check(editor->SetToAll(edited.p), response, "Apply edited fields to supported tag formats")) return true;
    if (!Check(editor->Save(), response, "Save file tags")) {
        // Native disk writes are not promised transactional. Never automatically retry.
        response["file_may_have_changed"] = true;
        response["library_changed"] = true;
        return true;
    }
    json changed = json::array();
    for (auto entry = cmd["fields"].begin(); entry != cmd["fields"].end(); ++entry) changed.push_back(entry.key());
    response = {{"ok", true}, {"library_changed", true}, {"tags", {{"file_path", filename}, {"saved", true}, {"changed_fields", std::move(changed)}}}};
    if (item) {
        const auto refreshed = item->ReloadInfo();
        if (FAILED(refreshed)) response["metadata_refresh_error"] = static_cast<int32_t>(refreshed);
    }
    return true;
}
