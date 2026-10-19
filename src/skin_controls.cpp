#include "skin_controls.h"
#include "globals.h"
#include "utils.h"
#include "apiSkins.h"
#include <cmath>

namespace {
// Audited against official 5.40 apiSkins.pas and CHM: all methods used here
// have matching stdcall declarations. Select must run on the AIMP main thread.
template<class T> struct Ref {
    T* p = nullptr;
    ~Ref() { if (p) p->Release(); }
    Ref() = default;
    Ref(const Ref&) = delete;
    Ref& operator=(const Ref&) = delete;
    T* operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
    T** out() { if (p) p->Release(); p = nullptr; return &p; }
    void** outVoid() { return reinterpret_cast<void**>(out()); }
};
constexpr int MaxSkins = 512;
constexpr std::size_t MaxInventoryBytes = 1024 * 1024;
json installed = json::array();
bool inventoryLoaded = false;
bool inventoryAvailable = false;

json Failure(const char* code, HRESULT hr = S_OK) {
    json error{{"code", code}};
    if (hr != S_OK) error["hresult"] = static_cast<std::int32_t>(hr);
    return {{"error", error}};
}
bool MainThread() { return g_core && GetCurrentThreadId() == g_mainThreadId; }
bool Service(Ref<IAIMPServiceSkinsManager>& service) {
    return g_core && g_core->QueryInterface(IID_IAIMPServiceSkinsManager, service.outVoid()) == S_OK && service;
}
bool Properties(IAIMPServiceSkinsManager* service, Ref<IAIMPPropertyList>& properties) {
    return service && service->QueryInterface(IID_IAIMPPropertyList, properties.outVoid()) == S_OK && properties;
}
bool StringValue(IAIMPString* value, std::string& text, int maximum) {
    if (!value) return false;
    const int length = value->GetLength();
    const wchar_t* data = value->GetData();
    if (length < 0 || length > maximum || (!data && length)) return false;
    std::wstring wide(data ? data : L"", length);
    if (wide.find(L'\0') != std::wstring::npos) return false;
    text = WStrToUtf8(wide);
    return true;
}
json TextProperty(IAIMPPropertyList* properties, int id, int maximum = 2048) {
    Ref<IAIMPString> value;
    std::string text;
    return properties && properties->GetValueAsObject(id, IID_IAIMPString, value.outVoid()) == S_OK && StringValue(value.p, text, maximum)
        ? json(text) : json(nullptr);
}
bool InstalledName(const std::string& name) {
    if (name.empty() || name.size() > 2048 || name == "." || name == "..") return false;
    for (unsigned char c : name) if (c < 32 || c == 127 || c == '/' || c == '\\' || c == ':') return false;
    return true;
}
HRESULT RefreshInventory(IAIMPServiceSkinsManager* service) {
    inventoryLoaded = true;
    inventoryAvailable = false;
    installed = json::array();
    Ref<IAIMPObjectList> list;
    HRESULT hr = service ? service->EnumSkins(list.out()) : E_NOINTERFACE;
    if (hr != S_OK || !list) return hr == S_OK ? E_UNEXPECTED : hr;
    const int count = list->GetCount();
    if (count < 0 || count > MaxSkins) return E_UNEXPECTED;
    json next = json::array();
    std::set<std::string> seen;
    std::size_t bytes = 2;
    for (int i = 0; i < count; ++i) {
        Ref<IAIMPString> name;
        hr = list->GetObject(i, IID_IAIMPString, name.outVoid());
        std::string filename;
        if (hr != S_OK || !StringValue(name.p, filename, 512) || !InstalledName(filename)) return hr == S_OK ? E_UNEXPECTED : hr;
        if (!seen.insert(filename).second) continue;
        json item{{"name", filename}, {"title", filename}, {"author", nullptr}, {"description", nullptr}};
        Ref<IAIMPSkinInfo> info;
        if (service->GetSkinInfo(name.p, info.out()) == S_OK && info) {
            json title = TextProperty(info.p, AIMP_SKININFO_PROPID_NAME, 512);
            if (title.is_string() && !title.get_ref<const std::string&>().empty()) item["title"] = title;
            item["author"] = TextProperty(info.p, AIMP_SKININFO_PROPID_AUTHOR, 512);
            item["description"] = TextProperty(info.p, AIMP_SKININFO_PROPID_DESCRIPTION);
        }
        bytes += item.dump().size() + 1;
        if (bytes > MaxInventoryBytes) return E_OUTOFMEMORY;
        next.push_back(std::move(item));
    }
    installed = std::move(next);
    inventoryAvailable = true;
    return S_OK;
}
json NumberProperty(IAIMPPropertyList* properties, int id, int maximum) {
    int value = -1;
    return properties && properties->GetValueAsInt32(id, &value) == S_OK && value >= 0 && value <= maximum ? json(value) : json(nullptr);
}
json State(IAIMPServiceSkinsManager* service) {
    Ref<IAIMPPropertyList> properties;
    Properties(service, properties);
    if (service && !inventoryLoaded) RefreshInventory(service);
    return {{"available", service != nullptr}, {"current", TextProperty(properties.p, AIMP_SERVICE_SKINSMAN_PROPID_SKIN, 512)},
        {"installed", service ? installed : json::array()}, {"list_available", service && inventoryAvailable},
        {"hue", NumberProperty(properties.p, AIMP_SERVICE_SKINSMAN_PROPID_HUE, 255)},
        {"intensity", NumberProperty(properties.p, AIMP_SERVICE_SKINSMAN_PROPID_HUE_INTENSITY, 100)}};
}
bool MakeString(const std::string& text, Ref<IAIMPString>& value) {
    std::wstring wide = Utf8ToWStr(text);
    return (text.empty() || !wide.empty()) && g_core->CreateObject(IID_IAIMPString, value.outVoid()) == S_OK && value &&
        value->SetData(wide.data(), static_cast<int>(wide.size())) == S_OK;
}
}

json GetSkinCapabilities() {
    if (!MainThread()) return {{"skins", {{"available", false}, {"read_error", "AIMP_MAIN_THREAD_REQUIRED"}}}};
    Ref<IAIMPServiceSkinsManager> service;
    Service(service);
    const json state = State(service.p);
    return {{"skins", {{"available", bool(service)}, {"list", state["list_available"]}, {"selection", bool(service)},
        {"install", false}, {"uninstall", false}, {"max_installed", MaxSkins}, {"max_inventory_bytes", MaxInventoryBytes},
        {"color", {{"hue", {{"available", !state["hue"].is_null()}, {"min", 0}, {"max", 255}, {"step", 1}}},
                   {"intensity", {{"available", !state["intensity"].is_null()}, {"min", 0}, {"max", 100}, {"step", 1}}}}}}}};
}
json GetSkinState() {
    if (!MainThread()) return {{"skins", {{"available", false}, {"read_error", "AIMP_MAIN_THREAD_REQUIRED"}}}};
    Ref<IAIMPServiceSkinsManager> service;
    Service(service);
    return {{"skins", State(service.p)}};
}
bool HandleSkinCommand(const std::string& action, const json& command, json& response) {
    if (action != "skins/get" && action != "skins/set" && action != "skins/reapply" &&
        action != "skins/color/set" && action != "skins/color/adjust") return false;
    if (!MainThread()) { response = Failure("AIMP_MAIN_THREAD_REQUIRED"); return true; }
    if (!command.is_object()) { response = Failure("INVALID_COMMAND"); return true; }
    Ref<IAIMPServiceSkinsManager> service;
    if (!Service(service)) { response = Failure("SKINS_UNAVAILABLE"); return true; }
    HRESULT result = S_OK;
    std::string colorParameter;
    if (action == "skins/get") {
        result = RefreshInventory(service.p);
    } else if (action == "skins/set") {
        if (!command.contains("name") || !command["name"].is_string()) { response = Failure("INVALID_SKIN_NAME"); return true; }
        const std::string name = command["name"].get<std::string>();
        if (!name.empty()) {
            if (!InstalledName(name)) { response = Failure("INVALID_SKIN_NAME"); return true; }
            result = RefreshInventory(service.p);
            if (result != S_OK) { response = Failure("SKIN_LIST_UNAVAILABLE", result); return true; }
            bool found = false;
            for (const auto& item : installed) if (item["name"] == name) { found = true; break; }
            if (!found) { response = Failure("SKIN_NOT_INSTALLED"); return true; }
        }
        Ref<IAIMPString> target;
        if (!MakeString(name, target)) { response = Failure("INVALID_SKIN_NAME"); return true; }
        result = service->Select(target.p);
    } else if (action == "skins/reapply") {
        result = service->Select(nullptr);
    } else {
        if (!command.contains("parameter") || !command["parameter"].is_string()) { response = Failure("INVALID_SKIN_PARAMETER"); return true; }
        const std::string parameter = command["parameter"].get<std::string>();
        if (parameter != "hue" && parameter != "intensity") { response = Failure("INVALID_SKIN_PARAMETER"); return true; }
        colorParameter = parameter;
        const int id = parameter == "hue" ? AIMP_SERVICE_SKINSMAN_PROPID_HUE : AIMP_SERVICE_SKINSMAN_PROPID_HUE_INTENSITY;
        const int maximum = parameter == "hue" ? 255 : 100;
        Ref<IAIMPPropertyList> properties;
        if (!Properties(service.p, properties)) { response = Failure("SKIN_COLOR_UNAVAILABLE"); return true; }
        const json current = NumberProperty(properties.p, id, maximum);
        if (!current.is_number_integer()) { response = Failure("SKIN_COLOR_UNAVAILABLE"); return true; }
        const bool relative = action == "skins/color/adjust";
        const char* key = relative ? "delta" : "value";
        if (!command.contains(key) || !command[key].is_number_integer()) { response = Failure("INVALID_SKIN_COLOR_VALUE"); return true; }
        const double value = command[key].get<double>();
        if (!std::isfinite(value) || (!relative && (value < 0 || value > maximum))) { response = Failure("INVALID_SKIN_COLOR_VALUE"); return true; }
        const int target = relative ? static_cast<int>(std::clamp(current.get<double>() + value, 0.0, static_cast<double>(maximum))) : static_cast<int>(value);
        result = properties->SetValueAsInt32(id, target);
        if (result == S_OK) result = service->Select(nullptr);
    }
    response = {{"skins", State(service.p)}};
    if (result != S_OK) response.update(Failure(action == "skins/get" ? "SKIN_LIST_UNAVAILABLE" : "SKIN_OPERATION_FAILED", result));
    else if (action != "skins/get" && (response["skins"]["current"].is_null() ||
        (!colorParameter.empty() && response["skins"][colorParameter].is_null()))) response.update(Failure("SKIN_READBACK_FAILED"));
    return true;
}
