#include "media_controls.h"
#include "media_helpers.h"
#include "globals.h"
#include "utils.h"
#include "protocol.h"
#include "commands.h"
#include "apiAlbumArt.h"
#include "apiLyrics.h"
#include <bcrypt.h>
#include <atomic>
#include <cstring>
#include <memory>

namespace {
using namespace media_control;

template<class T> class Ref {
    T* value_ = nullptr;
public:
    Ref() = default;
    Ref(const Ref&) = delete;
    Ref& operator=(const Ref&) = delete;
    ~Ref() { reset(); }
    void reset() { if (value_) value_->Release(); value_ = nullptr; }
    void retain(T* value) { if (value) value->AddRef(); reset(); value_ = value; }
    T* get() const { return value_; }
    T* operator->() const { return value_; }
    explicit operator bool() const { return value_ != nullptr; }
    T** out() { reset(); return &value_; }
    void** outVoid() { return reinterpret_cast<void**>(out()); }
};

template<class T> bool Service(REFIID iid, Ref<T>& value) {
    return g_core && g_core->QueryInterface(iid, value.outVoid()) == S_OK && value;
}

json Error(const char* code, HRESULT result = S_OK) {
    json error{{"code", code}};
    if (result != S_OK) error["hresult"] = static_cast<std::int32_t>(result);
    return {{"error", error}};
}

// Image encoders receive a real bounded stream, so rejecting an oversized
// result happens during serialization rather than after an unbounded allocation.
class BoundedStream final : public IAIMPStream {
    std::atomic<ULONG> references_{1};
    std::vector<unsigned char> bytes_;
    std::size_t position_ = 0;
    const std::size_t limit_;
public:
    explicit BoundedStream(std::size_t limit) : limit_(limit) {}
    HRESULT WINAPI QueryInterface(REFIID iid, void** output) override {
        if (!output) return E_POINTER;
        *output = nullptr;
        if (IsEqualIID(iid, IID_IUnknown) || IsEqualIID(iid, IID_IAIMPStream)) {
            *output = static_cast<IAIMPStream*>(this); AddRef(); return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG WINAPI AddRef() override { return ++references_; }
    ULONG WINAPI Release() override { const auto count = --references_; if (!count) delete this; return count; }
    INT64 WINAPI GetSize() override { return static_cast<INT64>(bytes_.size()); }
    HRESULT WINAPI SetSize(const INT64 size) override {
        if (size < 0 || static_cast<std::uint64_t>(size) > limit_) return E_INVALIDARG;
        try { bytes_.resize(static_cast<std::size_t>(size)); position_ = std::min(position_, bytes_.size()); return S_OK; }
        catch (...) { return E_OUTOFMEMORY; }
    }
    INT64 WINAPI GetPosition() override { return static_cast<INT64>(position_); }
    HRESULT WINAPI Seek(const INT64 offset, int mode) override {
        INT64 base = 0;
        if (mode == AIMP_STREAM_SEEKMODE_FROM_CURRENT) base = static_cast<INT64>(position_);
        else if (mode == AIMP_STREAM_SEEKMODE_FROM_END) base = static_cast<INT64>(bytes_.size());
        else if (mode != AIMP_STREAM_SEEKMODE_FROM_BEGINNING) return E_INVALIDARG;
        if (offset < -base || offset > static_cast<INT64>(limit_) - base) return E_INVALIDARG;
        position_ = static_cast<std::size_t>(base + offset); return S_OK;
    }
    int WINAPI Read(unsigned char* buffer, unsigned int count) override {
        if (count && !buffer) return -1;
        const auto available = position_ < bytes_.size() ? bytes_.size() - position_ : 0;
        const auto size = std::min<std::size_t>(count, available);
        if (size) std::memcpy(buffer, bytes_.data() + position_, size);
        position_ += size; return static_cast<int>(size);
    }
    HRESULT WINAPI Write(unsigned char* buffer, unsigned int count, unsigned int* written) override {
        if (written) *written = 0;
        if ((count && !buffer) || position_ > limit_ || count > limit_ - position_) return E_INVALIDARG;
        try {
            if (position_ + count > bytes_.size()) bytes_.resize(position_ + count);
            if (count) std::memcpy(bytes_.data() + position_, buffer, count);
            position_ += count; if (written) *written = count; return S_OK;
        } catch (...) { return E_OUTOFMEMORY; }
    }
    const std::vector<unsigned char>& bytes() const { return bytes_; }
};

std::string StringValue(IAIMPString* text, std::size_t maximum, bool* truncated = nullptr) {
    if (!text) return {};
    const int length = text->GetLength();
    const wchar_t* data = text->GetData();
    if (length <= 0 || !data) return {};
    std::size_t count = std::min<std::size_t>(length, maximum);
    if (count && count < static_cast<std::size_t>(length) && data[count - 1] >= 0xd800 && data[count - 1] <= 0xdbff) --count;
    std::string result = WStrToUtf8(std::wstring(data, count));
    if (truncated && (count < static_cast<std::size_t>(length) || result.size() > maximum)) *truncated = true;
    return Utf8Prefix(std::move(result), maximum);
}

std::string PropertyText(IAIMPPropertyList* properties, int property, std::size_t maximum, bool* truncated = nullptr) {
    Ref<IAIMPString> text;
    return properties && properties->GetValueAsObject(property, IID_IAIMPString, text.outVoid()) == S_OK
        ? StringValue(text.get(), maximum, truncated) : std::string{};
}

enum class RequestKind { Artwork, Lyrics };
struct Session;
struct Request {
    Session* owner;
    RequestKind kind;
    std::string key;
    Ref<IAIMPFileInfo> info;
    TTaskHandle task = 0;
    std::atomic<bool> complete{false};
    std::atomic<bool> wrongThread{false};
    bool obsolete = false;
    bool submitting = false;
    ULONGLONG started = 0;
    Request(Session* session, RequestKind type, std::string identity) : owner(session), kind(type), key(std::move(identity)) {}
};

struct Session {
    DWORD thread = 0;
    std::atomic<bool> closing{false};
    Ref<IAIMPServiceAlbumArt> artService;
    Ref<IAIMPServiceLyrics> lyricsService;
    Ref<IAIMPFileInfo> fileInfo;
    Ref<IAIMPLyrics> lyricsObject;
    std::unique_ptr<Request> artRequest;
    std::unique_ptr<Request> lyricsRequest;
    std::vector<std::unique_ptr<Request>> abandoned;
    HMODULE moduleReference = nullptr;
    bool retainModule = false;
    bool servicesQueried = false;
    std::string key;
    json artwork{{"status", "idle"}, {"revision", 0}};
    json lyrics{{"status", "idle"}, {"type", "none"}, {"revision", 0}};
    int lyricCount = 0;
    std::uint64_t revision = 0;
    ULONGLONG lastArt = 0, lastLyrics = 0;
};
Session* active = nullptr;

bool MainThread() { return g_core && GetCurrentThreadId() == g_mainThreadId; }
bool Ready() {
    if (!MainThread() || !active || active->closing || !IsAimpLoaded()) return false;
    if (!active->servicesQueried) {
        Service(IID_IAIMPServiceAlbumArt, active->artService);
        Service(IID_IAIMPServiceLyrics, active->lyricsService);
        active->servicesQueried = true;
    }
    return true;
}

bool RetainModule(Session& session) {
    if (session.moduleReference) return true;
    return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
        reinterpret_cast<LPCWSTR>(&InitializeMediaControls), &session.moduleReference) != FALSE;
}

bool CancelRequest(Session& session, std::unique_ptr<Request>& request) {
    if (!request) return true;
    request->obsolete = true;
    if (request->submitting) {
        // AIMP may pump its main-thread queue while submitting a request.
        // The Get call still owns the out-task address: do not destroy it.
        session.retainModule = true;
        session.abandoned.push_back(std::move(request));
        return false;
    }
    HRESULT result = request->complete ? S_OK : E_UNEXPECTED;
    if (!request->complete && request->task) {
        if (request->kind == RequestKind::Artwork)
            result = session.artService ? session.artService->Cancel(request->task, AIMP_SERVICE_ALBUMART_FLAGS_WAITFOR) : E_NOINTERFACE;
        else result = session.lyricsService ? session.lyricsService->Cancel(request->task, AIMP_SERVICE_LYRICS_FLAGS_WAITFOR) : E_NOINTERFACE;
    }
    if ((result != S_OK && !request->complete) || request->wrongThread) {
        // A failed cancellation cannot prove the callback is gone. Keep its
        // context and our module loaded; never allow a callback into an unloaded DLL.
        session.retainModule = true;
        session.abandoned.push_back(std::move(request));
        return false;
    }
    request.reset(); return true;
}

std::string TrackKey(IAIMPFileInfo* info) {
    std::string source;
    for (const int property : {AIMP_FILEINFO_PROPID_FILENAME, AIMP_FILEINFO_PROPID_TITLE,
            AIMP_FILEINFO_PROPID_ARTIST, AIMP_FILEINFO_PROPID_ALBUM}) {
        const auto text = PropertyText(info, property, 16384);
        source += std::to_string(text.size()) + ':' + text;
    }
    unsigned char digest[32]{};
    if (BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0,
            reinterpret_cast<PUCHAR>(source.data()), static_cast<ULONG>(source.size()), digest, sizeof(digest)) != 0) return {};
    static constexpr char hex[] = "0123456789abcdef";
    std::string key; key.reserve(64);
    for (unsigned char byte : digest) { key += hex[byte >> 4]; key += hex[byte & 15]; }
    return key;
}

HRESULT CurrentTrack(Session& session) {
    Ref<IAIMPServicePlayer> player;
    if (!Service(IID_IAIMPServicePlayer, player)) return E_NOINTERFACE;
    Ref<IAIMPFileInfo> info;
    std::string key;
    if (player->GetState() != AIMP_PLAYER_STATE_STOPPED) {
        const HRESULT result = player->GetInfo(info.out());
        if (result != S_OK || !info) return result == S_OK ? E_UNEXPECTED : result;
        key = TrackKey(info.get());
        if (key.empty()) return E_UNEXPECTED;
    }
    if (key == session.key) {
        // A transient Clone failure must not permanently suppress this track.
        if (!key.empty() && !session.fileInfo) return info->Clone(session.fileInfo.out());
        return S_OK;
    }
    CancelRequest(session, session.artRequest);
    CancelRequest(session, session.lyricsRequest);
    session.lyricsObject.reset(); session.fileInfo.reset(); session.lyricCount = 0;
    session.key = key;
    session.artwork = {{"status", "idle"}, {"revision", ++session.revision}};
    session.lyrics = {{"status", "idle"}, {"type", "none"}, {"revision", session.revision}};
    session.lastArt = session.lastLyrics = 0;
    if (info) return info->Clone(session.fileInfo.out());
    return S_OK;
}

HRESULT EncodeArtwork(IAIMPImage* image, IAIMPImageContainer* container, json& result) {
    Ref<IAIMPImage> owned;
    SIZE size{};
    HRESULT status = E_FAIL;
    // The callback's Image is AIMP's prepared image. Prefer it over decoding
    // the original container, which can be much larger than the requested art.
    if (image) {
        status = image->GetSize(&size);
        if (status != S_OK || !ImageSizeAllowed(size.cx, size.cy)) return status == S_OK ? E_INVALIDARG : status;
        // IAIMPImage::Clone has a C++/Delphi calling-convention disagreement in
        // SDK 5.40. Copy through documented stdcall stream methods instead.
        Ref<IAIMPStream> copy;
        *copy.out() = new BoundedStream(MaxSourceImageBytes);
        status = image->SaveToStream(copy.get(), AIMP_IMAGE_FORMAT_PNG);
        if (status == S_OK) status = copy->Seek(0, AIMP_STREAM_SEEKMODE_FROM_BEGINNING);
        if (status == S_OK) status = g_core->CreateObject(IID_IAIMPImage, owned.outVoid());
        if (status == S_OK && owned) status = owned->LoadFromStream(copy.get());
    } else if (container) {
        int format = 0;
        if (container->GetDataSize() > MaxSourceImageBytes) return E_INVALIDARG;
        status = container->GetInfo(&size, &format);
        if (status != S_OK || !ImageSizeAllowed(size.cx, size.cy)) return status == S_OK ? E_INVALIDARG : status;
        status = container->CreateImage(owned.out());
    }
    if (status != S_OK || !owned) return status == S_OK ? E_UNEXPECTED : status;
    status = owned->GetSize(&size);
    if (status != S_OK || !ImageSizeAllowed(size.cx, size.cy)) return status == S_OK ? E_INVALIDARG : status;
    const auto dimensions = ThumbnailSize(size.cx, size.cy);
    if (size.cx != dimensions.first || size.cy != dimensions.second)
        status = owned->Resize(dimensions.first, dimensions.second);
    if (status != S_OK) return status;
    Ref<IAIMPStream> output;
    auto* bounded = new BoundedStream(MaxArtworkBytes);
    *output.out() = bounded;
    status = owned->SaveToStream(output.get(), AIMP_IMAGE_FORMAT_PNG);
    if (status != S_OK) return status;
    const auto& bytes = bounded->bytes();
    const unsigned char signature[] = {137, 80, 78, 71, 13, 10, 26, 10};
    if (bytes.size() < 8 || std::memcmp(bytes.data(), signature, 8) != 0) return E_UNEXPECTED;
    result = {{"status", "ready"}, {"mime", "image/png"}, {"width", dimensions.first}, {"height", dimensions.second},
              {"data_base64", protocol::Base64(bytes.data(), bytes.size())}};
    return S_OK;
}

json LyricLine(IAIMPLyrics* lyrics, int index) {
    if (!lyrics || index < 0) return nullptr;
    int start = 0, finish = 0;
    Ref<IAIMPString> text;
    if (lyrics->Get(index, &start, &finish, text.out()) != S_OK || !text) return nullptr;
    bool truncated = false;
    json line{{"index", index}, {"start_ms", start}, {"end_ms", finish},
              {"text", StringValue(text.get(), MaxLineBytes, &truncated)}};
    if (truncated) line["truncated"] = true;
    return line;
}

void ReadLyrics(Session& session, IAIMPLyrics* source) {
    session.lyricsObject.reset(); session.lyricCount = 0;
    if (!source) { session.lyrics = {{"status", "missing"}, {"type", "none"}}; return; }
    int type = 0, offset = 0, count = 0;
    HRESULT status = source->GetValueAsInt32(AIMP_LYRICS_PROPID_TYPE, &type);
    if (status == S_OK) status = source->GetCount(&count);
    if (status != S_OK || count < 0) {
        session.lyrics = {{"status", "error"}, {"type", "none"}, {"error", "LYRICS_READ_FAILED"}}; return;
    }
    const HRESULT offsetStatus = source->GetValueAsInt32(AIMP_LYRICS_PROPID_OFFSET, &offset);
    bool truncated = false;
    std::string text = PropertyText(source, AIMP_LYRICS_PROPID_TEXT, MaxLyricsBytes, &truncated);
    json lines = json::array();
    std::size_t total = 0;
    for (int index = 0; index < std::min(count, MaxLyricsLines); ++index) {
        json line = LyricLine(source, index);
        if (line.is_null()) { truncated = true; break; }
        const auto bytes = line["text"].get_ref<const std::string&>().size();
        if (bytes > MaxLyricsBytes - total) { truncated = true; break; }
        total += bytes; lines.push_back(std::move(line));
    }
    if (count > MaxLyricsLines) truncated = true;
    session.lyrics = {{"status", text.empty() && lines.empty() ? "missing" : "ready"},
        {"type", type == AIMP_LYRICS_TYPE_SYNCED ? "synced" : "plain"}, {"offset_ms", offset},
        {"line_count", count}, {"text", std::move(text)}, {"lines", std::move(lines)}, {"truncated", truncated}};
    if (offsetStatus != S_OK) session.lyrics["offset_ms"] = nullptr;
    session.lyricCount = count;
    // Callback argument is borrowed; retain exactly one reference for SDK Find.
    if (type == AIMP_LYRICS_TYPE_SYNCED) session.lyricsObject.retain(source);
}

void CALLBACK ArtworkReceived(IAIMPImage* image, IAIMPImageContainer* container, void* userdata) {
    auto* request = static_cast<Request*>(userdata);
    if (!request) return;
    auto& session = *request->owner;
    if (GetCurrentThreadId() != session.thread) { request->wrongThread = true; request->complete = true; return; }
    try {
        if (!session.closing && !request->obsolete && request->key == session.key) {
            if (!image && !container) session.artwork = {{"status", "missing"}};
            else {
                json encoded;
                HRESULT result = EncodeArtwork(image, container, encoded);
                session.artwork = result == S_OK ? std::move(encoded) : json{{"status", "error"},
                    {"error", "ARTWORK_CONVERSION_FAILED"}, {"hresult", static_cast<std::int32_t>(result)}};
            }
            session.artwork["revision"] = ++session.revision;
        }
    } catch (...) {
        try { session.artwork = {{"status", "error"}, {"error", "ARTWORK_CALLBACK_FAILED"}, {"revision", ++session.revision}}; } catch (...) {}
    }
    request->complete = true;
}

void CALLBACK LyricsReceived(IAIMPLyrics* lyrics, void* userdata) {
    auto* request = static_cast<Request*>(userdata);
    if (!request) return;
    auto& session = *request->owner;
    if (GetCurrentThreadId() != session.thread) { request->wrongThread = true; request->complete = true; return; }
    try {
        if (!session.closing && !request->obsolete && request->key == session.key) {
            ReadLyrics(session, lyrics);
            session.lyrics["revision"] = ++session.revision;
        }
    } catch (...) {
        try { session.lyrics = {{"status", "error"}, {"type", "none"}, {"error", "LYRICS_CALLBACK_FAILED"}, {"revision", ++session.revision}}; } catch (...) {}
    }
    request->complete = true;
}

void FinishRequest(Session& session, std::unique_ptr<Request>& request, json& state) {
    if (!request || request->submitting) return;
    if (request->wrongThread) {
        state = {{"status", "error"}, {"error", "MEDIA_CALLBACK_WRONG_THREAD"}, {"revision", ++session.revision}};
        CancelRequest(session, request);
        return;
    } else if (!request->complete && GetTickCount64() - request->started > 30000) {
        CancelRequest(session, request);
        state = {{"status", "error"}, {"error", "MEDIA_REQUEST_TIMEOUT"}, {"revision", ++session.revision}};
        return;
    }
    if (request && request->complete) request.reset();
}

HRESULT StartRequest(Session& session, RequestKind kind, bool refresh) {
    auto& request = kind == RequestKind::Artwork ? session.artRequest : session.lyricsRequest;
    auto& state = kind == RequestKind::Artwork ? session.artwork : session.lyrics;
    auto& last = kind == RequestKind::Artwork ? session.lastArt : session.lastLyrics;
    if (session.key.empty() || !session.fileInfo) return E_FAIL;
    if ((kind == RequestKind::Artwork && !session.artService) || (kind == RequestKind::Lyrics && !session.lyricsService)) return E_NOINTERFACE;
    if (!refresh && state.value("status", "idle") != "idle") return S_OK;
    if (last && GetTickCount64() - last < 1000) return S_FALSE;
    if (session.abandoned.size() >= 4 || !RetainModule(session)) return E_UNEXPECTED;
    if (!CancelRequest(session, request)) return E_UNEXPECTED;
    request = std::make_unique<Request>(&session, kind, session.key);
    HRESULT result = session.fileInfo->Clone(request->info.out());
    if (result != S_OK || !request->info) { request.reset(); return result == S_OK ? E_UNEXPECTED : result; }
    request->started = last = GetTickCount64();
    state = {{"status", "pending"}, {"revision", ++session.revision}};
    if (kind == RequestKind::Lyrics) { state["type"] = "none"; session.lyricsObject.reset(); }
    // The callback may run synchronously for a cache hit. Request storage must
    // remain alive until Get has returned and assigned its task identifier.
    Request* submitted = request.get();
    submitted->submitting = true;
    if (kind == RequestKind::Artwork)
        result = session.artService->Get2(submitted->info.get(), refresh ? AIMP_SERVICE_ALBUMART_FLAGS_NOCACHE : 0,
            &ArtworkReceived, submitted, &submitted->task);
    else result = session.lyricsService->Get(submitted->info.get(), refresh ? AIMP_SERVICE_LYRICS_FLAGS_NOCACHE : 0,
            &LyricsReceived, submitted, &submitted->task);
    submitted->submitting = false;
    if (submitted->obsolete || session.closing || request.get() != submitted) return E_ABORT;
    if (result != S_OK) {
        request->obsolete = true;
        CancelRequest(session, request);
        state = {{"status", "error"}, {"error", "MEDIA_REQUEST_FAILED"},
            {"hresult", static_cast<std::int32_t>(result)}, {"revision", ++session.revision}};
    }
    return result;
}

json LyricsState(Session& session, bool content) {
    json result = session.lyrics;
    result["track_key"] = session.key;
    if (!content) { result.erase("text"); result.erase("lines"); }
    result["current"] = nullptr; result["previous"] = nullptr; result["next"] = nullptr;
    if (session.lyricsObject && result.value("status", "idle") == "ready") {
        Ref<IAIMPServicePlayer> player;
        double seconds = 0;
        int milliseconds = 0, index = -1;
        Ref<IAIMPString> current;
        if (Service(IID_IAIMPServicePlayer, player) && player->GetPosition(&seconds) == S_OK && PlaybackMilliseconds(seconds, milliseconds) &&
            session.lyricsObject->Find(milliseconds, &index, current.out()) == S_OK && index >= 0 && index < session.lyricCount) {
            result["current"] = LyricLine(session.lyricsObject.get(), index);
            if (index > 0) result["previous"] = LyricLine(session.lyricsObject.get(), index - 1);
            if (index + 1 < session.lyricCount) result["next"] = LyricLine(session.lyricsObject.get(), index + 1);
        }
    }
    return result;
}

json Summary(Session& session) {
    FinishRequest(session, session.artRequest, session.artwork);
    FinishRequest(session, session.lyricsRequest, session.lyrics);
    json art = session.artwork;
    art.erase("data_base64"); art["track_key"] = session.key;
    return {{"track_key", session.key}, {"artwork", std::move(art)}, {"lyrics", LyricsState(session, false)}};
}
} // namespace

void InitializeMediaControls() {
    if (!MainThread() || active) return;
    auto session = std::make_unique<Session>();
    session->thread = GetCurrentThreadId();
    // Two live requests plus four failed cancellations fit without allocating
    // inside a cancellation/finalization failure path.
    session->abandoned.reserve(6);
    // AIMP can invoke plug-in Initialize before services have finished loading.
    // Query the optional media services lazily after AIMP_MSG_PROPERTY_LOADED.
    active = session.release();
}

void FinalizeMediaControls() {
    Session* session = active;
    if (!session) return;
    active = nullptr;
    session->closing = true;
    CancelRequest(*session, session->artRequest);
    CancelRequest(*session, session->lyricsRequest);
    session->lyricsObject.reset(); session->fileInfo.reset();
    if (session->retainModule) {
        // A failed SDK cancellation cannot prove callback completion. Retain
        // this bounded session and module reference until process exit.
        return;
    }
    const HMODULE module = session->moduleReference;
    delete session;
    // AIMP still owns its normal module reference while Finalize is executing.
    if (module) FreeLibrary(module);
}

json GetMediaCapabilities() {
    if (!Ready()) return {{"media", {{"artwork", false}, {"lyrics", false}, {"synchronized_lyrics", false}}}};
    return {{"media", {{"artwork", bool(active->artService)}, {"lyrics", bool(active->lyricsService)},
        {"synchronized_lyrics", bool(active->lyricsService)}, {"max_artwork_bytes", MaxArtworkBytes},
        {"max_artwork_dimension", MaxArtworkDimension}, {"max_lyrics_bytes", MaxLyricsBytes}, {"max_lyrics_lines", MaxLyricsLines}}}};
}

json GetMediaState() {
    auto failure = [](const char* code, HRESULT result = S_OK) {
        return json{{"media", {{"track_key", ""},
            {"artwork", {{"status", "error"}, {"track_key", ""}}},
            {"lyrics", {{"status", "error"}, {"type", "none"}, {"track_key", ""},
                {"current", nullptr}, {"previous", nullptr}, {"next", nullptr}}},
            {"read_error", Error(code, result)["error"]}}}};
    };
    if (!Ready()) return failure("MEDIA_UNAVAILABLE");
    const HRESULT result = CurrentTrack(*active);
    if (result != S_OK) return failure("MEDIA_TRACK_UNAVAILABLE", result);
    return {{"media", Summary(*active)}};
}

bool HandleMediaCommand(const std::string& action, const json& command, json& response) {
    if (action != "media/get" && action != "media/artwork/get" && action != "media/lyrics/get") return false;
    if (!Ready()) { response = Error("MEDIA_UNAVAILABLE"); return true; }
    if (!command.is_object() || (command.contains("expected_track_key") &&
        (!command["expected_track_key"].is_string() || command["expected_track_key"].get_ref<const std::string&>().size() > 128)) ||
        (command.contains("refresh") && !command["refresh"].is_boolean()) ||
        (command.contains("include_content") && !command["include_content"].is_boolean())) {
        response = Error("INVALID_MEDIA_PARAMETER"); return true;
    }
    const HRESULT current = CurrentTrack(*active);
    if (current != S_OK) { response = Error("MEDIA_TRACK_UNAVAILABLE", current); return true; }
    if (command.contains("expected_track_key") && command["expected_track_key"].get_ref<const std::string&>() != active->key) {
        response = Error("STALE_MEDIA_TRACK"); response["media"] = Summary(*active); return true;
    }
    if (action == "media/get") { response = {{"media", Summary(*active)}}; return true; }
    if (active->key.empty()) { response = Error("NO_PLAYING_MEDIA"); response["media"] = Summary(*active); return true; }
    FinishRequest(*active, active->artRequest, active->artwork);
    FinishRequest(*active, active->lyricsRequest, active->lyrics);
    const auto kind = action == "media/artwork/get" ? RequestKind::Artwork : RequestKind::Lyrics;
    Session* session = active;
    const HRESULT status = StartRequest(*session, kind, command.value("refresh", false));
    if (active != session || session->closing) { response = Error("MEDIA_UNAVAILABLE"); return true; }
    response = {{"media", {{"track_key", active->key}}}};
    if (kind == RequestKind::Artwork) { response["artwork"] = active->artwork; response["artwork"]["track_key"] = active->key; }
    else response["lyrics"] = LyricsState(*active, command.value("include_content", true));
    if (status != S_OK && status != S_FALSE) response.update(Error("MEDIA_REQUEST_FAILED", status));
    if (status == S_FALSE) response["refresh_deferred"] = true;
    return true;
}
