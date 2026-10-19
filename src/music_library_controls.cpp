#include "music_library_controls.h"
#include "music_library_validation.h"
#include "globals.h"
#include "utils.h"
#include "apiMusicLibrary.h"
#include <oleauto.h>
#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <new>
#include <utility>

namespace {
using namespace music_library;
template<class T> class Ref {
    T* p_ = nullptr;
public:
    Ref() = default;
    Ref(const Ref& other) : p_(other.p_) { if (p_) p_->AddRef(); }
    Ref(Ref&& other) noexcept : p_(other.p_) { other.p_ = nullptr; }
    Ref& operator=(Ref other) noexcept { std::swap(p_, other.p_); return *this; }
    ~Ref() { if (p_) p_->Release(); }
    T* get() const { return p_; }
    T* operator->() const { return p_; }
    explicit operator bool() const { return p_ != nullptr; }
    T** out() { if (p_) p_->Release(); p_ = nullptr; return &p_; }
    void** outVoid() { return reinterpret_cast<void**>(out()); }
    T** inout() { return &p_; }
    void attach(T* p) { if (p_) p_->Release(); p_ = p; }
};
template<class T> bool Service(REFIID id, Ref<T>& out) {
    return g_core && g_core->QueryInterface(id, out.outVoid()) == S_OK && out;
}
bool MainThread() { return g_core && GetCurrentThreadId() == g_mainThreadId; }
json Failure(const char* code, HRESULT hr = S_OK) {
    json error = {{"code", code}};
    if (hr != S_OK) error["hresult"] = static_cast<std::int32_t>(hr);
    return {{"error", error}};
}
bool Check(HRESULT hr, json& response, const char* code) {
    if (hr == S_OK) return true;
    response = Failure(code, hr); return false;
}
bool String(const std::string& text, Ref<IAIMPString>& value, json& response) {
    if (!g_core || !Check(g_core->CreateObject(IID_IAIMPString, value.outVoid()), response, "STRING_UNAVAILABLE") || !value) return false;
    auto wide = Utf8ToWStr(text);
    if (!text.empty() && wide.empty()) { response = Failure("INVALID_UTF8"); return false; }
    return Check(value->SetData(wide.data(), static_cast<int>(wide.size())), response, "STRING_UNAVAILABLE");
}
std::string Text(IAIMPString* value, int maxCharacters = 512) {
    if (!value || !value->GetData()) return {};
    int count = std::min(std::max(value->GetLength(), 0), maxCharacters);
    const auto* data = value->GetData();
    if (count && data[count - 1] >= 0xd800 && data[count - 1] <= 0xdbff) --count;
    return WStrToUtf8(std::wstring(data, count));
}
std::string PropertyText(IAIMPPropertyList* object, int property) {
    Ref<IAIMPString> value;
    return object && object->GetValueAsObject(property, IID_IAIMPString, value.outVoid()) == S_OK ? Text(value.get()) : "";
}
std::string FieldType(int type) {
    switch (type) {
    case AIMPML_FIELDTYPE_INT32: return "int32";
    case AIMPML_FIELDTYPE_INT64: return "int64";
    case AIMPML_FIELDTYPE_FLOAT: return "number";
    case AIMPML_FIELDTYPE_STRING: return "string";
    case AIMPML_FIELDTYPE_STRINGSET: return "string_set";
    case AIMPML_FIELDTYPE_DATETIME: return "datetime";
    case AIMPML_FIELDTYPE_DURATION: return "duration";
    case AIMPML_FIELDTYPE_FILESIZE: return "filesize";
    case AIMPML_FIELDTYPE_FILENAME: return "filename";
    default: return {};
    }
}
struct Storage {
    Ref<IAIMPMLDataStorage> base;
    Ref<IAIMPMLDataStorage2> factory;
    Ref<IAIMPMLDataProvider> provider;
    Ref<IAIMPMLDataProvider2> provider2;
    std::string id, name, schemaSource, reason;
    std::vector<Field> fields;
    bool searchable = false, sortable = false, queryable = false;
    bool cursor = false;
    json commands = json::object();
};

template<class T> bool Supports(IUnknown* object, REFIID iid) {
    Ref<T> command;
    return object && object->QueryInterface(iid, command.outVoid()) == S_OK && command;
}

// Never fabricate a provider or reinterpret its interface layout. GetFields is
// optional: some storage wrappers expose the extension, others expose only the
// public storage interface. Local storage has an official SDK field catalog.
void Discover(Storage& storage) {
    storage.id = PropertyText(storage.base.get(), AIMPML_DATASTORAGE_PROPID_ID);
    storage.name = PropertyText(storage.base.get(), AIMPML_DATASTORAGE_PROPID_CAPTION);
    storage.base->QueryInterface(IID_IAIMPMLDataStorage2, storage.factory.outVoid());
    storage.base->QueryInterface(IID_IAIMPMLDataProvider, storage.provider.outVoid());
    storage.base->QueryInterface(IID_IAIMPMLDataProvider2, storage.provider2.outVoid());
    storage.cursor = !storage.provider && bool(storage.provider2);
    storage.commands = {
        {"add_files", Supports<IAIMPMLDataStorageCommandAddFiles>(storage.base.get(), IID_IAIMPMLDataStorageCommandAddFiles)},
        {"add_files_dialog", Supports<IAIMPMLDataStorageCommandAddFilesDialog>(storage.base.get(), IID_IAIMPMLDataStorageCommandAddFilesDialog)},
        {"report_dialog", Supports<IAIMPMLDataStorageCommandReportDialog>(storage.base.get(), IID_IAIMPMLDataStorageCommandReportDialog)},
        {"reload_tags", Supports<IAIMPMLDataStorageCommandReloadTags>(storage.base.get(), IID_IAIMPMLDataStorageCommandReloadTags)},
        {"row_mark", Supports<IAIMPMLDataStorageCommandUserMark>(storage.base.get(), IID_IAIMPMLDataStorageCommandUserMark)}};
    storage.commands["row_reload_tags"] = storage.commands["reload_tags"];
    Ref<IAIMPMLDataStorageCommandDeleteFiles> remove;
    storage.commands["row_remove"] = storage.base->QueryInterface(IID_IAIMPMLDataStorageCommandDeleteFiles, remove.outVoid()) == S_OK && remove && remove->CanDelete(FALSE);
    Ref<IAIMPMLExtensionDataStorage> extension;
    Ref<IAIMPObjectList> schema;
    if (storage.base->QueryInterface(IID_IAIMPMLExtensionDataStorage, extension.outVoid()) == S_OK && extension &&
        extension->GetFields(AIMPML_FIELDS_SCHEMA_ALL, schema.out()) == S_OK && schema && schema->GetCount() >= 0 && schema->GetCount() <= MaxSchema) {
        std::set<std::string> names;
        for (int i = 0; i < schema->GetCount(); ++i) {
            Ref<IAIMPMLDataField> field;
            if (schema->GetObject(i, IID_IAIMPMLDataField, field.outVoid()) != S_OK || !field) continue;
            auto id = PropertyText(field.get(), AIMPML_FIELD_PROPID_NAME);
            int type = 0, flags = 0;
            if (id.empty() || id.size() > 128 || !names.insert(id).second || field->GetValueAsInt32(AIMPML_FIELD_PROPID_TYPE, &type) != S_OK) continue;
            if (field->GetValueAsInt32(AIMPML_FIELD_PROPID_FLAGS, &flags) != S_OK) flags = 0;
            auto kind = FieldType(type);
            if (!kind.empty()) storage.fields.push_back({id, kind, bool(flags & AIMPML_FIELDFLAG_FILTERING), bool(flags & AIMPML_FIELDFLAG_INTERNAL)});
        }
        if (!storage.fields.empty()) storage.schemaSource = "provider";
    }
    if (storage.fields.empty() && storage.id == WStr(AIMPML_LOCALDATASTORAGE_ID)) {
        storage.fields = LocalSchema(); storage.schemaSource = "official_local_catalog";
    }
    Ref<IAIMPMLDataFilter> filter;
    bool filterAvailable = storage.factory && storage.factory->CreateObject(IID_IAIMPMLDataFilter, filter.outVoid()) == S_OK && filter;
    Ref<IAIMPServiceThreads> threads;
    bool threadsAvailable = Service(IID_IAIMPServiceThreads, threads);
    storage.queryable = !storage.id.empty() && (storage.provider || storage.provider2) && filterAvailable && threadsAvailable &&
        FindField(storage.fields, "ID") && FindField(storage.fields, "FileName");
    if (!storage.queryable) {
        storage.reason = !storage.factory ? "No provider-bound filter factory" :
            (!storage.provider && !storage.provider2) ? "No data provider" :
            !filterAvailable ? "Provider filters unavailable" :
            !threadsAvailable ? "Background task service unavailable" : "Required field schema unavailable";
    }
    for (const auto& field : storage.fields) if (field.filterable && IsString(field) && !field.internal) storage.searchable = true;
    storage.sortable = filterAvailable;
}
json Descriptor(const Storage& storage) {
    json result = {{"id", storage.id}, {"name", storage.name.empty() ? storage.id : storage.name}, {"can_query", storage.queryable},
        {"schema_source", storage.schemaSource}, {"paging", storage.cursor ? "cursor" : "offset"},
        {"search_supported", storage.searchable}, {"sort_supported", storage.sortable}, {"fields_available", !storage.fields.empty()}, {"commands", storage.commands}};
    if (!storage.reason.empty()) result["unavailable_reason"] = storage.reason;
    return result;
}
bool GetStorage(const std::string& id, Storage& storage, json& response) {
    Ref<IAIMPServiceMusicLibrary> service;
    if (!Service(IID_IAIMPServiceMusicLibrary, service)) { response = Failure("MUSIC_LIBRARY_UNAVAILABLE"); return false; }
    Ref<IAIMPString> name;
    if (!String(id, name, response)) return false;
    if (!Check(service->GetStorageByID(name.get(), IID_IAIMPMLDataStorage, storage.base.outVoid()), response, "STORAGE_NOT_FOUND") || !storage.base) return false;
    Discover(storage);
    return true;
}
json Storages() {
    Ref<IAIMPServiceMusicLibrary> service;
    json result = {{"storages", json::array()}, {"active_storage", nullptr}};
    if (!Service(IID_IAIMPServiceMusicLibrary, service)) { result["available"] = false; return result; }
    result["available"] = true;
    Ref<IAIMPMLDataStorage> active;
    if (service->GetActiveStorage(IID_IAIMPMLDataStorage, active.outVoid()) == S_OK && active)
        result["active_storage"] = PropertyText(active.get(), AIMPML_DATASTORAGE_PROPID_ID);
    int count = service->GetStorageCount();
    if (count < 0 || count > MaxStorages) { result["error"] = {{"code", "INVALID_STORAGE_COUNT"}}; return result; }
    for (int i = 0; i < count; ++i) {
        Storage storage;
        if (service->GetStorage(i, IID_IAIMPMLDataStorage, storage.base.outVoid()) != S_OK || !storage.base) continue;
        Discover(storage);
        auto descriptor = Descriptor(storage);
        descriptor["active"] = result["active_storage"].is_string() && storage.id == result["active_storage"].get<std::string>();
        result["storages"].push_back(std::move(descriptor));
    }
    return result;
}
json Fields(const Storage& storage) {
    json result = {{"storage_id", storage.id}, {"schema_source", storage.schemaSource}, {"search_supported", storage.searchable},
        {"sort_supported", storage.sortable}, {"fields", json::array()}};
    for (const auto& field : storage.fields) {
        json operators = json::array();
        if (field.filterable) {
            operators = {"eq", "ne", "lt", "lte", "gt", "gte", "between"};
            if (IsString(field)) for (const char* op : {"contains", "starts_with", "ends_with"}) operators.push_back(op);
        }
        result["fields"].push_back({{"id", field.id}, {"label", field.id}, {"type", field.type}, {"filterable", field.filterable},
            {"internal", field.internal}, {"operators", operators}});
    }
    return result;
}

// The filter sort-list contract requires IAIMPString + IAIMPMLSortDirection on
// the SAME object. Delegate string operations to an ordinary SDK string and
// implement the documented sorting interface; no custom data provider exists.
class SortField final : public IAIMPString, public IAIMPMLSortDirection {
    LONG references_ = 1;
    Ref<IAIMPString> name_;
    int direction_;
public:
    SortField(const Ref<IAIMPString>& name, int direction) : name_(name), direction_(direction) {}
    HRESULT WINAPI QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER; *out = nullptr;
        if (iid == IID_IUnknown || iid == IID_IAIMPString) *out = static_cast<IAIMPString*>(this);
        else if (iid == IID_IAIMPMLSortDirection) *out = static_cast<IAIMPMLSortDirection*>(this);
        else return E_NOINTERFACE;
        AddRef(); return S_OK;
    }
    ULONG WINAPI AddRef() override { return InterlockedIncrement(&references_); }
    ULONG WINAPI Release() override { LONG n = InterlockedDecrement(&references_); if (!n) delete this; return n; }
    HRESULT WINAPI GetValue(int* value) override { if (!value) return E_POINTER; *value = direction_; return S_OK; }
    HRESULT WINAPI SetValue(int value) override { if (value != 1 && value != 2) return E_INVALIDARG; direction_ = value; return S_OK; }
    HRESULT WINAPI GetChar(int i, TChar* c) override { return name_->GetChar(i, c); }
    TChar* WINAPI GetData() override { return name_->GetData(); }
    int WINAPI GetLength() override { return name_->GetLength(); }
    int WINAPI GetHashCode() override { return name_->GetHashCode(); }
    HRESULT WINAPI SetChar(int i, TChar c) override { return name_->SetChar(i, c); }
    HRESULT WINAPI SetData(TChar* c, int n) override { return name_->SetData(c, n); }
    HRESULT WINAPI Add(IAIMPString* s) override { return name_->Add(s); }
    HRESULT WINAPI Add2(TChar* s, int n) override { return name_->Add2(s, n); }
    HRESULT WINAPI ChangeCase(int mode) override { return name_->ChangeCase(mode); }
    HRESULT WINAPI Clone(IAIMPString** s) override {
        if (!s) return E_POINTER;
        *s = nullptr;
        Ref<IAIMPString> copy;
        HRESULT hr = name_->Clone(copy.out());
        if (hr != S_OK || !copy) return hr == S_OK ? E_UNEXPECTED : hr;
        *s = new (std::nothrow) SortField(copy, direction_);
        return *s ? S_OK : E_OUTOFMEMORY;
    }
    HRESULT WINAPI Compare(IAIMPString* s, int* result, BOOL ignore) override { return name_->Compare(s, result, ignore); }
    HRESULT WINAPI Compare2(TChar* s, int n, int* result, BOOL ignore) override { return name_->Compare2(s, n, result, ignore); }
    HRESULT WINAPI Delete(int i, int n) override { return name_->Delete(i, n); }
    HRESULT WINAPI Find(IAIMPString* s, int* i, int flags, int start) override { return name_->Find(s, i, flags, start); }
    HRESULT WINAPI Find2(TChar* s, int n, int* i, int flags, int start) override { return name_->Find2(s, n, i, flags, start); }
    HRESULT WINAPI Insert(int i, IAIMPString* s) override { return name_->Insert(i, s); }
    HRESULT WINAPI Insert2(int i, TChar* s, int n) override { return name_->Insert2(i, s, n); }
    HRESULT WINAPI Replace(IAIMPString* a, IAIMPString* b, int flags) override { return name_->Replace(a, b, flags); }
    HRESULT WINAPI Replace2(TChar* a, int an, TChar* b, int bn, int flags) override { return name_->Replace2(a, an, b, bn, flags); }
    HRESULT WINAPI SubString(int i, int n, IAIMPString** s) override { return name_->SubString(i, n, s); }
};
struct Variant {
    VARIANT value;
    Variant() { VariantInit(&value); }
    ~Variant() { VariantClear(&value); }
    Variant(const Variant&) = delete;
    Variant& operator=(const Variant&) = delete;
    bool set(const Field& field, const json& input) {
        if (IsString(field)) {
            const auto& utf8 = input.get_ref<const std::string&>();
            auto text = Utf8ToWStr(utf8);
            if (!utf8.empty() && text.empty()) return false;
            value.vt = VT_BSTR; value.bstrVal = SysAllocStringLen(text.data(), static_cast<UINT>(text.size()));
            return value.bstrVal != nullptr || text.empty();
        }
        if (field.type == "int32") { value.vt = VT_I4; value.lVal = input.get<LONG>(); }
        else if (field.type == "int64" || field.type == "filesize") {
            value.vt = VT_I8;
            value.llVal = input.is_string() ? std::stoll(input.get<std::string>()) : input.get<LONGLONG>();
        }
        else { value.vt = VT_R8; value.dblVal = input.get<double>(); }
        return true;
    }
};
int Operation(const std::string& name) {
    static const std::map<std::string, int> ids{{"eq", 0}, {"ne", 1}, {"between", 2}, {"lt", 3}, {"lte", 4},
        {"gt", 5}, {"gte", 6}, {"contains", 7}, {"starts_with", 8}, {"ends_with", 9}};
    auto item = ids.find(name); return item == ids.end() ? -1 : item->second;
}
bool AddFilter(IAIMPMLDataFilterGroup* group, const Field& field, const json& first, const json* second, int operation, json& response) {
    Ref<IAIMPString> name;
    if (!String(field.id, name, response)) return false;
    Variant a, b;
    b.value.vt = VT_NULL;
    if (!a.set(field, first) || (second && !b.set(field, *second))) { response = Failure("OUT_OF_MEMORY"); return false; }
    Ref<IAIMPMLDataFieldFilter> child;
    return Check(group->Add(name.get(), &a.value, &b.value, operation, child.out()), response, "FILTER_REJECTED");
}

struct Context {
    Ref<IAIMPMLDataProvider> provider;
    Ref<IAIMPMLDataProvider2> provider2;
    Ref<IAIMPMLDataFilter> filter;
    Ref<IAIMPObjectList> fields;
    Ref<IUnknown> page;
    Ref<IAIMPMLDataProviderSelection> remaining;
    Request request;
    std::vector<Field> columns;
    bool cursor = false, canPlay = false, canAdd = false, canQueue = false;
    json commands = json::object();
};
bool Prepare(const Request& request, Context& context, json& response) {
    Storage storage;
    if (!GetStorage(request.storage, storage, response)) return false;
    if (!storage.queryable) { response = Failure("STORAGE_QUERY_UNAVAILABLE"); response["error"]["detail"] = storage.reason; return false; }
    auto error = ValidateSchema(request, storage.fields);
    if (!error.empty()) { response = Failure(error.c_str()); return false; }
    if (storage.cursor && request.offset != 0) { response = Failure("CURSOR_STORAGE_REQUIRES_FIRST_PAGE"); return false; }
    context.request = request; context.cursor = storage.cursor;
    context.commands = storage.commands;
    context.provider = storage.provider; context.provider2 = storage.provider2;
    if (!Check(storage.factory->CreateObject(IID_IAIMPMLDataFilter, context.filter.outVoid()), response, "FILTER_UNAVAILABLE") || !context.filter) return false;
    if (!Check(context.filter->SetValueAsInt32(AIMPML_FILTER_LIMIT, request.limit + (storage.cursor ? 0 : 1)), response, "PAGING_UNAVAILABLE") ||
        !Check(context.filter->SetValueAsInt32(AIMPML_FILTER_OFFSET, request.offset), response, "PAGING_UNAVAILABLE") ||
        !Check(context.filter->SetValueAsInt32(AIMPML_FILTERGROUP_OPERATION, AIMPML_FILTERGROUP_OPERATION_AND), response, "FILTER_UNAVAILABLE")) return false;
    if (!request.filters.empty()) {
        Ref<IAIMPMLDataFilterGroup> group;
        if (!Check(context.filter->AddGroup(group.out()), response, "FILTER_UNAVAILABLE") || !group ||
            !Check(group->SetValueAsInt32(AIMPML_FILTERGROUP_OPERATION, request.any ? AIMPML_FILTERGROUP_OPERATION_OR : AIMPML_FILTERGROUP_OPERATION_AND), response, "FILTER_UNAVAILABLE")) return false;
        for (const auto& f : request.filters) {
            const auto* field = FindField(storage.fields, f["field"].get<std::string>());
            const json* second = f["operator"] == "between" ? &f["value2"] : nullptr;
            if (!AddFilter(group.get(), *field, f["value"], second, Operation(f["operator"].get<std::string>()), response)) return false;
        }
    }
    if (!request.search.empty()) {
        Ref<IAIMPMLDataFilterGroup> group;
        if (!Check(context.filter->AddGroup(group.out()), response, "SEARCH_UNAVAILABLE") || !group ||
            !Check(group->SetValueAsInt32(AIMPML_FILTERGROUP_OPERATION, AIMPML_FILTERGROUP_OPERATION_OR), response, "SEARCH_UNAVAILABLE")) return false;
        int count = 0;
        // A bounded OR of supported string fields, independent of the optional
        // provider SEARCHSTRING property. Prefer useful metadata to file paths.
        for (const char* name : {"Title", "Artist", "Album", "AlbumArtist", "Genre", "FileName"}) {
            const auto* field = FindField(storage.fields, name);
            if (field && field->filterable && IsString(*field)) { if (!AddFilter(group.get(), *field, request.search, nullptr, 7, response)) return false; ++count; }
        }
        if (!count) for (const auto& field : storage.fields) {
            if (count >= 6) break;
            if (field.filterable && IsString(field) && !field.internal) { if (!AddFilter(group.get(), field, request.search, nullptr, 7, response)) return false; ++count; }
        }
        if (!count) { response = Failure("SEARCH_UNAVAILABLE"); return false; }
    }
    if (!request.sort.empty()) {
        Ref<IAIMPObjectList> order;
        if (!Check(g_core->CreateObject(IID_IAIMPObjectList, order.outVoid()), response, "SORT_UNAVAILABLE") || !order) return false;
        for (const auto& entry : request.sort) {
            Ref<IAIMPString> name;
            if (!String(entry["field"].get<std::string>(), name, response)) return false;
            Ref<IAIMPString> sorter;
            sorter.attach(new SortField(name, entry["direction"] == "desc" ? 2 : 1));
            if (!Check(order->Add(sorter.get()), response, "SORT_UNAVAILABLE")) return false;
        }
        if (!Check(context.filter->SetValueAsObject(AIMPML_FILTER_SORTBYLIST, order.get()), response, "SORT_UNAVAILABLE")) return false;
    }
    if (!Check(g_core->CreateObject(IID_IAIMPObjectList, context.fields.outVoid()), response, "FIELDS_UNAVAILABLE") || !context.fields) return false;
    std::vector<std::string> names = {"ID", "FileName"};
    if (!request.fields.empty()) names.insert(names.end(), request.fields.begin(), request.fields.end());
    else for (const char* id : {"Title", "Artist", "Album", "Duration", "UserMark", "Year"}) if (FindField(storage.fields, id)) names.emplace_back(id);
    std::set<std::string> seen;
    for (const auto& name : names) {
        if (!seen.insert(name).second) continue;
        const auto* field = FindField(storage.fields, name);
        if (!field || context.columns.size() >= MaxFields) continue;
        Ref<IAIMPString> text;
        if (!String(name, text, response) || !Check(context.fields->Add(text.get()), response, "FIELDS_UNAVAILABLE")) return false;
        context.columns.push_back(*field);
    }
    Ref<IAIMPServicePlayer> player;
    Ref<IAIMPServicePlaylistManager> playlists;
    Ref<IAIMPPlaylistQueue> queue;
    context.canPlay = Service(IID_IAIMPServicePlayer, player);
    context.canAdd = Service(IID_IAIMPServicePlaylistManager, playlists);
    context.canQueue = context.canAdd && playlists->QueryInterface(IID_IAIMPPlaylistQueue, queue.outVoid()) == S_OK && queue;
    return true;
}

struct Job {
    std::string id;
    Context context;
    std::string phase = "pending";
    json result;
    std::vector<std::wstring> paths;
    Ref<IUnknown> nextPage;
    Ref<IAIMPMLDataProviderSelection> remaining;
    bool consumed = false;
    std::atomic<bool> cancel{false};
    bool submitting = false;
    TTaskHandle handle = 0;
};
std::mutex jobsMutex;
std::deque<std::shared_ptr<Job>> jobs;
std::shared_ptr<Job> activeJob;
std::uint64_t nextJob = 1;
bool finalizing = false;
HMODULE workerModuleReference = nullptr;

std::wstring SelectionText(IAIMPMLDataProviderSelection* selection, int index, int maximum, bool* complete = nullptr) {
    int length = 0;
    const TChar* text = selection->GetValueAsString(index, &length);
    if (complete) *complete = text && length > 0 && length <= maximum;
    if (!text || length <= 0 || length > 1024 * 1024) return {};
    int count = std::min(length, maximum);
    if (count && text[count - 1] >= 0xd800 && text[count - 1] <= 0xdbff) --count;
    return std::wstring(text, count);
}
json Cell(IAIMPMLDataProviderSelection* selection, const Field& field, int index) {
    if (IsString(field)) return WStrToUtf8(SelectionText(selection, index, 128));
    if (field.type == "int32") return selection->GetValueAsInt32(index);
    if (field.type == "int64" || field.type == "filesize") return std::to_string(selection->GetValueAsInt64(index));
    const double number = selection->GetValueAsFloat(index);
    return std::isfinite(number) ? json(number) : json(nullptr);
}
void Finish(const std::shared_ptr<Job>& job, json result, const std::string& phase) {
    std::lock_guard<std::mutex> lock(jobsMutex);
    job->result = std::move(result); job->phase = phase;
}
class QueryTask final : public IAIMPTask {
    LONG references_ = 1;
    std::shared_ptr<Job> job_;
    bool Cancelled(IAIMPTaskOwner* owner) const { return job_->cancel.load() || (owner && owner->IsCanceled()); }
public:
    explicit QueryTask(std::shared_ptr<Job> job) : job_(std::move(job)) {}
    HRESULT WINAPI QueryInterface(REFIID id, void** out) override {
        if (!out) return E_POINTER; *out = nullptr;
        if (id != IID_IUnknown && id != IID_IAIMPTask) return E_NOINTERFACE;
        *out = static_cast<IAIMPTask*>(this); AddRef(); return S_OK;
    }
    ULONG WINAPI AddRef() override { return InterlockedIncrement(&references_); }
    ULONG WINAPI Release() override { LONG n = InterlockedDecrement(&references_); if (!n) delete this; return n; }
    void WINAPI Execute(IAIMPTaskOwner* owner) override {
        const auto& c = job_->context;
        json result = {{"id", job_->id}, {"storage_id", c.request.storage}, {"offset", c.request.offset}, {"limit", c.request.limit},
            {"paging", c.cursor ? "cursor" : "offset"}, {"rows", json::array()}, {"count", 0}, {"total", nullptr},
            {"has_more", false}, {"next_cursor", nullptr}, {"next_offset", nullptr}, {"queue_requires_loaded", true}};
        try {
            if (Cancelled(owner)) { result["status"] = "cancelled"; Finish(job_, std::move(result), "cancelled"); return; }
            Ref<IUnknown> data, page = c.page;
            Ref<IAIMPMLDataProviderSelection> selection = c.remaining;
            HRESULT hr = S_OK;
            if (!selection) {
                if (c.cursor) hr = c.provider2->GetData(c.fields.get(), c.filter.get(), nullptr, page.inout(), data.out());
                else hr = c.provider->GetData(c.fields.get(), c.filter.get(), data.out());
                if (hr != S_OK) { result["status"] = "failed"; result["error"] = Failure("QUERY_FAILED", hr)["error"]; Finish(job_, std::move(result), "failed"); return; }
                if (data) data->QueryInterface(IID_IAIMPMLDataProviderSelection, selection.outVoid());
            }
            bool moreRows = false;
            if (selection) {
                // Official MusicLibraryBrowserDemo reads the initial current row
                // before NextRow. Do not skip the first record.
                for (int rowIndex = 0; rowIndex < c.request.limit; ++rowIndex) {
                    if (Cancelled(owner)) break;
                    json values = json::object();
                    std::wstring path;
                    bool pathComplete = false;
                    json recordId = nullptr;
                    for (size_t fieldIndex = 0; fieldIndex < c.columns.size(); ++fieldIndex) {
                        const auto& field = c.columns[fieldIndex];
                        values[field.id] = Cell(selection.get(), field, static_cast<int>(fieldIndex));
                        if (field.id == "FileName") path = SelectionText(selection.get(), static_cast<int>(fieldIndex), 32768, &pathComplete);
                        if (field.id == "ID") {
                            if (IsString(field)) {
                                bool complete = false;
                                auto id = SelectionText(selection.get(), static_cast<int>(fieldIndex), 512, &complete);
                                if (complete && id.find(L'\0') == std::wstring::npos) recordId = WStrToUtf8(id);
                            } else recordId = values[field.id];
                        }
                    }
                    const bool actionable = pathComplete && !path.empty() && path.find(L'\0') == std::wstring::npos;
                    auto displayPath = path.substr(0, 1024);
                    if (!displayPath.empty() && displayPath.back() >= 0xd800 && displayPath.back() <= 0xdbff) displayPath.pop_back();
                    json row = {{"row_id", job_->id + ":" + std::to_string(rowIndex)}, {"record_id", recordId},
                        {"file_path", WStrToUtf8(displayPath)}, {"file_path_truncated", !pathComplete || path.size() > displayPath.size()},
                        {"title", values.value("Title", json(""))}, {"artist", values.value("Artist", json(""))}, {"album", values.value("Album", json(""))},
                        {"duration", values.value("Duration", json(nullptr))}, {"values", values},
                        {"can_play", actionable && c.canPlay}, {"can_add", actionable && c.canAdd}, {"can_queue", actionable && c.canQueue},
                        {"can_remove", actionable && !recordId.is_null() && c.commands.value("row_remove", false)},
                        {"can_reload_tags", actionable && !recordId.is_null() && c.commands.value("row_reload_tags", false)},
                        {"can_mark", !recordId.is_null() && c.commands.value("row_mark", false)}};
                    result["rows"].push_back(std::move(row));
                    job_->paths.push_back(actionable ? std::move(path) : std::wstring{});
                    moreRows = selection->NextRow() != FALSE;
                    if (!moreRows) break;
                }
            } else if (data) {
                Ref<IAIMPString> message;
                if (data->QueryInterface(IID_IAIMPString, message.outVoid()) == S_OK && message) result["provider_message"] = Text(message.get(), 2048);
                else { result["status"] = "failed"; result["error"] = {{"code", "UNSUPPORTED_QUERY_RESULT"}}; Finish(job_, std::move(result), "failed"); return; }
            }
            if (Cancelled(owner)) { result["status"] = "cancelled"; result["rows"] = json::array(); Finish(job_, std::move(result), "cancelled"); return; }
            const int count = static_cast<int>(result["rows"].size());
            result["count"] = count;
            result["status"] = "complete";
            result["has_more"] = moreRows || (c.cursor && bool(page));
            if (c.cursor && result["has_more"].get<bool>()) {
                job_->nextPage = page;
                if (moreRows) job_->remaining = selection;
                result["next_cursor"] = job_->id + ":next";
            } else if (!c.cursor && moreRows && c.request.offset + count <= MaxOffset) result["next_offset"] = c.request.offset + count;
            Finish(job_, std::move(result), "complete");
        } catch (...) {
            result["status"] = "failed"; result["error"] = {{"code", "QUERY_EXCEPTION"}};
            Finish(job_, std::move(result), "failed");
        }
    }
};
std::shared_ptr<Job> FindJob(const std::string& id) {
    for (const auto& job : jobs) if (job->id == id) return job;
    return {};
}
json JobResult(const std::shared_ptr<Job>& job) {
    if (job->phase == "pending") return {{"id", job->id}, {"status", "pending"}, {"storage_id", job->context.request.storage},
        {"offset", job->context.request.offset}, {"limit", job->context.request.limit}, {"paging", job->context.cursor ? "cursor" : "offset"}};
    return job->result;
}
json StartQuery(const json& command) {
    Request request;
    const auto error = ParseRequest(command, request);
    if (!error.empty()) return Failure(error.c_str());
    Ref<IAIMPServiceThreads> threads;
    if (!Service(IID_IAIMPServiceThreads, threads)) return Failure("QUERY_THREADS_UNAVAILABLE");
    std::shared_ptr<Job> previous;
    {
        std::lock_guard<std::mutex> lock(jobsMutex);
        if (finalizing) return Failure("PLUGIN_STOPPING");
        if (activeJob && activeJob->phase == "pending") return Failure("LIBRARY_QUERY_BUSY");
        previous = activeJob;
    }
    // Execute never queues work back to the GUI thread. Thus a completed task
    // may be joined without the cross-thread callback deadlock in naive designs.
    if (previous && previous->handle && threads->WaitFor(previous->handle) != S_OK) return Failure("PREVIOUS_QUERY_JOIN_FAILED");
    auto job = std::make_shared<Job>();
    json response;
    std::shared_ptr<Job> cursorSource;
    if (!request.cursor.empty()) {
        const std::string suffix = ":next";
        if (request.cursor.size() <= suffix.size() || request.cursor.substr(request.cursor.size() - suffix.size()) != suffix) return Failure("INVALID_CURSOR");
        std::lock_guard<std::mutex> lock(jobsMutex);
        cursorSource = FindJob(request.cursor.substr(0, request.cursor.size() - suffix.size()));
        if (!cursorSource || cursorSource->phase != "complete" || cursorSource->consumed ||
            (!cursorSource->nextPage && !cursorSource->remaining)) return Failure("CURSOR_EXPIRED");
        job->context = cursorSource->context;
        job->context.page = cursorSource->nextPage;
        job->context.remaining = cursorSource->remaining;
        job->context.request.offset += cursorSource->result["count"].get<int>();
        cursorSource->consumed = true;
    } else if (!Prepare(request, job->context, response)) return response;
    {
        std::lock_guard<std::mutex> lock(jobsMutex);
        job->id = "ml-" + std::to_string(nextJob++);
        jobs.push_back(job); activeJob = job;
        while (jobs.size() > MaxCachedPages) jobs.pop_front();
    }
    Ref<IAIMPTask> task;
    // Hold a module reference while AIMP can enter our worker's vtable. If the
    // task service cannot confirm teardown, Finalize retains this bounded state.
    if (!workerModuleReference && !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
        reinterpret_cast<LPCWSTR>(&FinalizeMusicLibraryControls), &workerModuleReference)) {
        std::lock_guard<std::mutex> lock(jobsMutex);
        job->phase = "failed"; job->result = {{"id", job->id}, {"status", "failed"}, {"error", {{"code", "WORKER_MODULE_REFERENCE_FAILED"}}}};
        if (cursorSource) cursorSource->consumed = false;
        return Failure("WORKER_MODULE_REFERENCE_FAILED");
    }
    task.attach(new QueryTask(job));
    job->submitting = true;
    const HRESULT hr = threads->ExecuteInThread(task.get(), &job->handle);
    job->submitting = false;
    if (hr != S_OK) {
        std::lock_guard<std::mutex> lock(jobsMutex);
        job->handle = 0; // The SDK only defines the out handle on successful submission.
        job->phase = "failed"; job->result = {{"id", job->id}, {"status", "failed"}, {"error", Failure("QUERY_START_FAILED", hr)["error"]}};
        if (cursorSource) cursorSource->consumed = false;
        return Failure("QUERY_START_FAILED", hr);
    }
    std::lock_guard<std::mutex> lock(jobsMutex);
    return {{"music_library", {{"query", JobResult(job)}}}};
}
bool RequestedJob(const json& command, std::shared_ptr<Job>& job, json& response) {
    if (!command.contains("query_id") || !SafeText(command["query_id"], 80)) { response = Failure("INVALID_QUERY_ID"); return false; }
    job = FindJob(command["query_id"].get<std::string>());
    if (!job) { response = Failure("QUERY_EXPIRED"); return false; }
    return true;
}

constexpr UINT OperationMessage = WM_APP + 117;
constexpr wchar_t OperationClass[] = L"AimpCompanionControl.MusicLibrary.Commands.v3";
HWND operationWindow = nullptr;
bool operationBusy = false, operationExecuting = false;
std::uint64_t nextOperation = 1;
std::function<HRESULT()> queuedOperation;
json lastOperation = nullptr;

LRESULT CALLBACK OperationProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message != OperationMessage) return DefWindowProcW(window, message, wParam, lParam);
    if (!operationBusy || !queuedOperation || !g_core) return 0;
    auto execute = std::move(queuedOperation);
    operationExecuting = true;
    lastOperation["status"] = "executing";
    try {
        HRESULT hr = execute();
        lastOperation["status"] = hr == S_OK ? "completed" : "failed";
        lastOperation["hresult"] = static_cast<std::int32_t>(hr);
        if (hr != S_OK) lastOperation["error"] = {{"code", "STORAGE_OPERATION_FAILED"}, {"hresult", static_cast<std::int32_t>(hr)}};
    } catch (...) {
        lastOperation["status"] = "failed";
        lastOperation["error"] = {{"code", "STORAGE_OPERATION_EXCEPTION"}};
    }
    operationExecuting = false; operationBusy = false;
    return 0;
}
json QueueOperation(const std::string& storage, const std::string& command, std::function<HRESULT()> execute) {
    if (finalizing) return Failure("PLUGIN_STOPPING");
    if (operationBusy) return Failure("STORAGE_OPERATION_BUSY");
    if (!workerModuleReference && !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
        reinterpret_cast<LPCWSTR>(&FinalizeMusicLibraryControls), &workerModuleReference)) return Failure("WORKER_MODULE_REFERENCE_FAILED");
    if (!operationWindow) {
        WNDCLASSW wc{}; wc.lpfnWndProc = OperationProc; wc.hInstance = g_hInstance; wc.lpszClassName = OperationClass;
        if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return Failure("STORAGE_OPERATION_QUEUE_FAILED");
        operationWindow = CreateWindowExW(0, OperationClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, g_hInstance, nullptr);
        if (!operationWindow) return Failure("STORAGE_OPERATION_QUEUE_FAILED");
    }
    operationBusy = true; queuedOperation = std::move(execute);
    lastOperation = {{"id", "ml-op-" + std::to_string(nextOperation++)}, {"status", "queued"}, {"command", command}, {"storage_id", storage}};
    if (!PostMessageW(operationWindow, OperationMessage, 0, 0)) {
        operationBusy = false; queuedOperation = {};
        lastOperation["status"] = "failed"; return Failure("STORAGE_OPERATION_QUEUE_FAILED");
    }
    return {{"acknowledgement", "queued"}, {"music_library", {{"operation", lastOperation}}}};
}
json StorageCommand(const json& command) {
    if (!command.contains("storage_id") || !SafeText(command["storage_id"], 512) ||
        !command.contains("command") || !SafeText(command["command"], 40)) return Failure("INVALID_STORAGE_COMMAND");
    Storage storage; json response;
    if (!GetStorage(command["storage_id"].get<std::string>(), storage, response)) return response;
    const auto name = command["command"].get<std::string>();
    if (name == "add_files") {
        if (!command.contains("files") || !command["files"].is_array() || command["files"].empty() || command["files"].size() > 100) return Failure("INVALID_IMPORT_FILES");
        Ref<IAIMPMLDataStorageCommandAddFiles> action;
        if (storage.base->QueryInterface(IID_IAIMPMLDataStorageCommandAddFiles, action.outVoid()) != S_OK || !action) return Failure("STORAGE_COMMAND_UNAVAILABLE");
        Ref<IAIMPObjectList> files;
        if (!Check(g_core->CreateObject(IID_IAIMPObjectList, files.outVoid()), response, "FILES_UNAVAILABLE") || !files) return response;
        for (const auto& file : command["files"]) {
            if (!SafeText(file, 32768)) return Failure("INVALID_IMPORT_FILES");
            Ref<IAIMPString> text;
            if (!String(file.get<std::string>(), text, response) || !Check(files->Add(text.get()), response, "INVALID_IMPORT_FILES")) return response;
        }
        return QueueOperation(storage.id, name, [action, files] { return action->Add(files.get()); });
    }
    if (name == "reload_tags") {
        Ref<IAIMPMLDataStorageCommandReloadTags> action;
        if (storage.base->QueryInterface(IID_IAIMPMLDataStorageCommandReloadTags, action.outVoid()) != S_OK || !action) return Failure("STORAGE_COMMAND_UNAVAILABLE");
        return QueueOperation(storage.id, name, [action] { return action->ReloadTags(nullptr); });
    }
    if (name == "add_files_dialog") {
        Ref<IAIMPMLDataStorageCommandAddFilesDialog> action;
        if (storage.base->QueryInterface(IID_IAIMPMLDataStorageCommandAddFilesDialog, action.outVoid()) != S_OK || !action) return Failure("STORAGE_COMMAND_UNAVAILABLE");
        return QueueOperation(storage.id, name, [action] { return action->Execute(nullptr); });
    }
    if (name == "report_dialog") {
        Ref<IAIMPMLDataStorageCommandReportDialog> action;
        if (storage.base->QueryInterface(IID_IAIMPMLDataStorageCommandReportDialog, action.outVoid()) != S_OK || !action) return Failure("STORAGE_COMMAND_UNAVAILABLE");
        return QueueOperation(storage.id, name, [action] { return action->Execute(nullptr); });
    }
    return Failure("UNKNOWN_STORAGE_COMMAND");
}
json RowStorageCommand(const std::string& actionName, const json& command) {
    json response, id; Field idField; std::wstring path; std::string storageId;
    {
        std::lock_guard<std::mutex> lock(jobsMutex);
        std::shared_ptr<Job> job;
        if (!RequestedJob(command, job, response)) return response;
        if (job->phase != "complete") return Failure("QUERY_NOT_COMPLETE");
        if (!command.contains("row_id") || !SafeText(command["row_id"], 100)) return Failure("INVALID_ROW_ID");
        storageId = job->context.request.storage;
        const auto* field = FindField(job->context.columns, "ID");
        if (!field) return Failure("ROW_ID_UNAVAILABLE");
        idField = *field;
        const int i = RowIndex(job->id, command["row_id"], job->result["rows"]);
        if (i >= 0 && static_cast<size_t>(i) < job->paths.size()) {
            path = job->paths[i]; id = job->result["rows"][i]["record_id"];
        }
    }
    if (id.is_null()) return Failure("ROW_ID_UNAVAILABLE");
    auto record = std::make_shared<Variant>();
    if (!record->set(idField, id)) return Failure("OUT_OF_MEMORY");
    Storage storage;
    if (!GetStorage(storageId, storage, response)) return response;
    if (actionName == "musiclibrary/row/mark") {
        if (!command.contains("value") || !command["value"].is_number() || !std::isfinite(command["value"].get<double>()) ||
            command["value"].get<double>() < 0 || command["value"].get<double>() > 5) return Failure("INVALID_USER_MARK");
        const double value = command["value"].get<double>();
        Ref<IAIMPMLDataStorageCommandUserMark> action;
        if (storage.base->QueryInterface(IID_IAIMPMLDataStorageCommandUserMark, action.outVoid()) != S_OK || !action) return Failure("STORAGE_COMMAND_UNAVAILABLE");
        return QueueOperation(storageId, "row_mark", [action, record, value] { return action->SetMark(&record->value, value); });
    }
    if (path.empty()) return Failure("ROW_NOT_PLAYABLE");
    Ref<IAIMPString> uri; Ref<IAIMPMLFileList> files;
    if (!String(WStrToUtf8(path), uri, response) || !Check(g_core->CreateObject(IID_IAIMPMLFileList, files.outVoid()), response, "FILES_UNAVAILABLE") || !files ||
        !Check(files->Add(&record->value, uri.get()), response, "FILES_UNAVAILABLE")) return response;
    if (actionName == "musiclibrary/row/remove") {
        if (!command.contains("confirm") || !command["confirm"].is_boolean() || !command["confirm"].get<bool>()) return Failure("CONFIRMATION_REQUIRED");
        Ref<IAIMPMLDataStorageCommandDeleteFiles> action;
        if (storage.base->QueryInterface(IID_IAIMPMLDataStorageCommandDeleteFiles, action.outVoid()) != S_OK || !action || !action->CanDelete(FALSE)) return Failure("STORAGE_COMMAND_UNAVAILABLE");
        return QueueOperation(storageId, "row_remove", [action, files] { return action->CanDelete(FALSE) ? action->Delete(files.get(), FALSE) : E_ACCESSDENIED; });
    }
    Ref<IAIMPMLDataStorageCommandReloadTags> action;
    if (storage.base->QueryInterface(IID_IAIMPMLDataStorageCommandReloadTags, action.outVoid()) != S_OK || !action) return Failure("STORAGE_COMMAND_UNAVAILABLE");
    return QueueOperation(storageId, "row_reload_tags", [action, files] { return action->ReloadTags(files.get()); });
}
json LibraryUi(const json* command = nullptr) {
    Ref<IAIMPServiceMusicLibraryUI> ui;
    if (!Service(IID_IAIMPServiceMusicLibraryUI, ui)) return {{"music_library", {{"ui", {{"available", false}, {"path", nullptr}}}}}};
    if (command) {
        if (!command->contains("path") || !SafeText((*command)["path"], 4096, true)) return Failure("INVALID_GROUPING_PATH");
        Ref<IAIMPString> path; json response;
        if (!String((*command)["path"].get<std::string>(), path, response) || !Check(ui->SetGroupingFilterPath(path.get()), response, "GROUPING_PATH_FAILED")) return response;
    }
    Ref<IAIMPString> path;
    const HRESULT hr = ui->GetGroupingFilterPath(path.out());
    return {{"music_library", {{"ui", {{"available", true}, {"path", hr == S_OK && path ? json(Text(path.get(), 4096)) : json(nullptr)}}}}}};
}
json RowAction(const std::string& action, const json& command) {
    std::wstring path;
    {
        std::lock_guard<std::mutex> lock(jobsMutex);
        std::shared_ptr<Job> job;
        json error;
        if (!RequestedJob(command, job, error)) return error;
        if (job->phase != "complete") return Failure("QUERY_NOT_COMPLETE");
        if (!command.contains("row_id") || !SafeText(command["row_id"], 100)) return Failure("INVALID_ROW_ID");
        const int i = RowIndex(job->id, command["row_id"], job->result["rows"]);
        if (i >= 0 && static_cast<size_t>(i) < job->paths.size()) path = job->paths[i];
    }
    if (path.empty()) return Failure("ROW_NOT_PLAYABLE");
    json response;
    Ref<IAIMPString> uri;
    if (!String(WStrToUtf8(path), uri, response)) return response;
    if (action == "musiclibrary/row/play") {
        Ref<IAIMPServicePlayer> player;
        if (!Service(IID_IAIMPServicePlayer, player)) return Failure("PLAYER_UNAVAILABLE");
        HRESULT hr = player->Play4(uri.get(), AIMP_SERVICE_PLAYER_FLAGS_PLAY_WITHOUT_ADDING_TO_PLAYLIST);
        return hr == S_OK ? json{{"accepted", true}} : Failure("PLAY_FAILED", hr);
    }
    Ref<IAIMPServicePlaylistManager> playlists;
    if (!Service(IID_IAIMPServicePlaylistManager, playlists)) return Failure("PLAYLISTS_UNAVAILABLE");
    if (action == "musiclibrary/row/add") {
        Ref<IAIMPPlaylist> playlist;
        HRESULT hr;
        if (command.contains("playlist_id")) {
            if (!Integer(command["playlist_id"], 0, std::numeric_limits<int>::max())) return Failure("INVALID_PLAYLIST_ID");
            hr = playlists->GetLoadedPlaylist(command["playlist_id"].get<int>(), playlist.out());
        } else hr = playlists->GetActivePlaylist(playlist.out());
        if (hr != S_OK || !playlist) return Failure("PLAYLIST_NOT_FOUND", hr);
        hr = playlist->Add(uri.get(), AIMP_PLAYLIST_ADD_FLAGS_NOEXPAND, -1);
        return hr == S_OK ? json{{"accepted", true}, {"library_changed", true}} : Failure("PLAYLIST_ADD_FAILED", hr);
    }
    if (command.contains("first") && !command["first"].is_boolean()) return Failure("INVALID_QUEUE_POSITION");
    const bool first = command.value("first", false);
    Ref<IAIMPPlaylistQueue> queue;
    if (playlists->QueryInterface(IID_IAIMPPlaylistQueue, queue.outVoid()) != S_OK || !queue) return Failure("QUEUE_UNAVAILABLE");
    int inspected = 0;
    const int count = playlists->GetLoadedPlaylistCount();
    for (int p = 0; p < std::min(count, 256); ++p) {
        Ref<IAIMPPlaylist> playlist;
        if (playlists->GetLoadedPlaylist(p, playlist.out()) != S_OK || !playlist) continue;
        for (int t = 0; t < playlist->GetItemCount(); ++t) {
            if (++inspected > MaxLoadedTracks) return Failure("LOADED_TRACK_SCAN_LIMIT");
            Ref<IAIMPPlaylistItem> item;
            Ref<IAIMPString> filename;
            if (playlist->GetItem(t, IID_IAIMPPlaylistItem, item.outVoid()) != S_OK || !item ||
                item->GetValueAsObject(AIMP_PLAYLISTITEM_PROPID_FILENAME, IID_IAIMPString, filename.outVoid()) != S_OK || !filename) continue;
            if (filename->GetLength() != static_cast<int>(path.size()) || !filename->GetData() ||
                std::wstring(filename->GetData(), filename->GetLength()) != path) continue;
            const HRESULT hr = queue->Add(item.get(), first ? TRUE : FALSE);
            return hr == S_OK ? json{{"ok", true}, {"queue", {{"count", queue->GetItemCount()}}}} : Failure("QUEUE_ADD_FAILED", hr);
        }
    }
    return Failure("ROW_NOT_LOADED_ADD_TO_PLAYLIST_FIRST");
}
const std::set<std::string> Commands{
    "musiclibrary/storages/get", "musiclibrary/storage/select", "musiclibrary/fields/get", "musiclibrary/query",
    "musiclibrary/query/get", "musiclibrary/query/cancel", "musiclibrary/row/play", "musiclibrary/row/add", "musiclibrary/row/queue",
    "musiclibrary/storage/command", "musiclibrary/operation/get", "musiclibrary/row/remove", "musiclibrary/row/mark",
    "musiclibrary/row/reload_tags", "musiclibrary/ui/get", "musiclibrary/ui/path/set"};
}

json GetMusicLibraryCapabilities() {
    if (!MainThread()) return {{"available", false}, {"reason", "MAIN_THREAD_REQUIRED"}};
    auto result = Storages();
    result["async"] = true;
    result["commands"] = Commands;
    result["limits"] = {{"page", MaxPage}, {"offset", MaxOffset}, {"fields", MaxFields - 2}, {"filters", MaxFilters}, {"sort", MaxSorts},
        {"cached_pages", MaxCachedPages}, {"loaded_track_scan", MaxLoadedTracks}, {"search_bytes", 512}};
    result["queue_requires_loaded"] = true;
    Ref<IAIMPServiceMusicLibraryUI> ui;
    result["ui"] = Service(IID_IAIMPServiceMusicLibraryUI, ui);
    result["physical_deletion"] = false;
    return result;
}

bool HandleMusicLibraryCommand(const std::string& action, const json& command, json& response) {
    if (!Commands.count(action)) return false;
    if (!MainThread()) { response = Failure("MAIN_THREAD_REQUIRED"); return true; }
    if (action == "musiclibrary/storage/command") response = StorageCommand(command);
    else if (action == "musiclibrary/operation/get") response = {{"music_library", {{"operation", lastOperation}}}};
    else if (action == "musiclibrary/row/remove" || action == "musiclibrary/row/mark" || action == "musiclibrary/row/reload_tags") response = RowStorageCommand(action, command);
    else if (action == "musiclibrary/ui/get") response = LibraryUi();
    else if (action == "musiclibrary/ui/path/set") response = LibraryUi(&command);
    else if (action == "musiclibrary/storages/get") response = {{"music_library", Storages()}};
    else if (action == "musiclibrary/fields/get" || action == "musiclibrary/storage/select") {
        if (!command.contains("storage_id") || !SafeText(command["storage_id"], 512)) { response = Failure("INVALID_STORAGE_ID"); return true; }
        Storage storage;
        if (!GetStorage(command["storage_id"].get<std::string>(), storage, response)) return true;
        if (action == "musiclibrary/fields/get") response = {{"music_library", {{"fields", Fields(storage)}}}};
        else {
            Ref<IAIMPServiceMusicLibrary> service;
            if (!Service(IID_IAIMPServiceMusicLibrary, service)) { response = Failure("MUSIC_LIBRARY_UNAVAILABLE"); return true; }
            const HRESULT hr = service->SetActiveStorage(storage.base.get());
            response = hr == S_OK ? json{{"music_library", Storages()}} : Failure("STORAGE_SELECT_FAILED", hr);
        }
    } else if (action == "musiclibrary/query") response = StartQuery(command);
    else if (action == "musiclibrary/query/get" || action == "musiclibrary/query/cancel") {
        std::shared_ptr<Job> job;
        {
            std::lock_guard<std::mutex> lock(jobsMutex);
            if (!RequestedJob(command, job, response)) return true;
            if (action == "musiclibrary/query/cancel" && job->phase == "pending") job->cancel.store(true);
            response = {{"music_library", {{"query", JobResult(job)}}}};
        }
        // Cooperative cancellation: cancelling a task before Execute begins
        // would otherwise leave its query permanently pending.
    } else response = RowAction(action, command);
    return true;
}

void FinalizeMusicLibraryControls() {
    std::shared_ptr<Job> job;
    {
        std::lock_guard<std::mutex> lock(jobsMutex);
        finalizing = true; job = activeJob;
        if (job) job->cancel.store(true);
    }
    if (operationWindow) { DestroyWindow(operationWindow); operationWindow = nullptr; }
    UnregisterClassW(OperationClass, g_hInstance);
    queuedOperation = {};
    operationBusy = operationExecuting;
    // AIMP's official browser demo uses Cancel(...WAITFOR) for task teardown.
    // No worker calls or synchronizes back to the GUI thread, so this cannot
    // deadlock on a queued main-thread callback. A misbehaving provider may
    // delay its own GetData return; the SDK offers no hard-abort operation.
    bool joined = !job || (!job->submitting && !job->handle);
    if (job && job->handle && !job->submitting && !joined) {
        Ref<IAIMPServiceThreads> threads;
        if (Service(IID_IAIMPServiceThreads, threads))
            joined = threads->Cancel(job->handle, AIMP_SERVICE_THREADS_FLAGS_WAITFOR) == S_OK || threads->WaitFor(job->handle) == S_OK;
    }
    if (!joined || operationExecuting) return; // Retain module + contexts if SDK execution cannot be joined.
    {
        std::lock_guard<std::mutex> lock(jobsMutex);
        jobs.clear(); activeJob.reset(); finalizing = false;
    }
    HMODULE module = workerModuleReference; workerModuleReference = nullptr;
    if (module) FreeLibrary(module); // AIMP still owns its plugin reference here.
}
