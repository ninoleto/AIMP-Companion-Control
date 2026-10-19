#include "app_controls.h"
#include "globals.h"
#include "utils.h"
#include "apiActions.h"
#include "ui_lifetime.h"
#include <cmath>

namespace {
template<class T> struct Ref {
    T* p = nullptr;
    Ref() = default;
    Ref(const Ref&) = delete;
    ~Ref() { if (p) p->Release(); }
    T* operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
    void** out() { return reinterpret_cast<void**>(&p); }
};
template<class T> HRESULT Service(REFIID id, Ref<T>& result) {
    return g_core ? g_core->QueryInterface(id, result.out()) : E_UNEXPECTED;
}
json Error(const char* code, HRESULT hr = E_INVALIDARG) {
    return {{"error", {{"code", code}, {"hresult", static_cast<int32_t>(hr)}}}};
}
bool Text(const json& command, const char* key, size_t max, bool empty = false) {
    if (!command.contains(key) || !command[key].is_string()) return false;
    const auto& value = command[key].get_ref<const std::string&>();
    return (empty || !value.empty()) && value.size() <= max && value.find('\0') == std::string::npos;
}
HRESULT SdkString(const std::string& text, Ref<IAIMPString>& value) {
    auto wide = Utf8ToWStr(text);
    if (!text.empty() && wide.empty()) return E_INVALIDARG;
    HRESULT hr = g_core->CreateObject(IID_IAIMPString, value.out());
    return hr == S_OK && value ? value->SetData(wide.data(), static_cast<int>(wide.size())) : hr;
}
HRESULT Lookup(const std::string& id, Ref<IAIMPAction>& action) {
    Ref<IAIMPServiceActionManager> service;
    HRESULT hr = Service(IID_IAIMPServiceActionManager, service);
    if (hr != S_OK || !service) return hr;
    Ref<IAIMPString> name;
    hr = SdkString(id, name);
    return hr == S_OK ? service->GetByID(name.p, &action.p) : hr;
}
json ActionInfo(const std::string& id, IAIMPAction* action) {
    json info = {{"id", id}, {"enabled", nullptr}, {"name", ""}, {"group", ""}, {"executable", false}};
    int enabled = 0;
    if (action->GetValueAsInt32(AIMP_ACTION_PROPID_ENABLED, &enabled) == S_OK) info["enabled"] = enabled != 0;
    for (const auto& field : {std::pair<const char*, int>{"name", AIMP_ACTION_PROPID_NAME}, {"group", AIMP_ACTION_PROPID_GROUPNAME}}) {
        Ref<IAIMPString> value;
        if (action->GetValueAsObject(field.second, IID_IAIMPString, value.out()) == S_OK && value)
            info[field.first] = WStr(value->GetData());
    }
    Ref<IAIMPActionEvent> event;
    info["executable"] = action->GetValueAsObject(AIMP_ACTION_PROPID_EVENT, IID_IAIMPActionEvent, event.out()) == S_OK && event;
    return info;
}
constexpr wchar_t ActionWindowClass[] = L"AimpCompanionControl.Actions.v3";
constexpr UINT ActionMessage = WM_APP + 98;
HWND actionWindow = nullptr;
bool actionBusy = false;
std::string pendingAction;
json lastAction = nullptr;
std::wstring notificationText;

LRESULT CALLBACK ActionProc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    if (message != ActionMessage) return DefWindowProcW(window, message, wp, lp);
    if (!actionBusy || !g_core) return 0;
    UiCallbackLease lease;
    if (!lease) {
        lastAction = {{"status", "failed"}, {"error", "MODULE_LIFETIME_UNAVAILABLE"}};
        actionBusy = false; pendingAction.clear(); return 0;
    }
    const std::string id = pendingAction;
    try {
        Ref<IAIMPAction> action;
        HRESULT hr = Lookup(id, action);
        int enabled = 0;
        if (hr == S_OK && action) hr = action->GetValueAsInt32(AIMP_ACTION_PROPID_ENABLED, &enabled);
        if (hr == S_OK && !enabled) hr = E_ACCESSDENIED;
        Ref<IAIMPActionEvent> event;
        if (hr == S_OK && action) hr = action->GetValueAsObject(AIMP_ACTION_PROPID_EVENT, IID_IAIMPActionEvent, event.out());
        if (hr == S_OK && event) {
            lastAction = {{"id", id}, {"status", "executing"}};
            // The SDK documents Data as the invoking IAIMPAction interface.
            event->OnExecute(action.p);
            if (!lease.current()) return 0;
            lastAction = {{"id", id}, {"status", "completed"}};
        } else lastAction = {{"id", id}, {"status", "failed"}, {"hresult", static_cast<int32_t>(hr == S_OK ? E_NOINTERFACE : hr)}};
    } catch (...) { if (lease.current()) lastAction = {{"id", id}, {"status", "failed"}, {"error", "AIMP_ACTION_EXCEPTION"}}; }
    if (!lease.current()) return 0;
    pendingAction.clear(); actionBusy = false;
    return 0;
}
bool EnsureActionWindow() {
    if (actionWindow) return true;
    WNDCLASSW wc{}; wc.lpfnWndProc = ActionProc; wc.hInstance = g_hInstance; wc.lpszClassName = ActionWindowClass;
    if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
    actionWindow = CreateWindowExW(0, ActionWindowClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, g_hInstance, nullptr);
    return actionWindow != nullptr;
}
int WindowId(const std::string& name) {
    if (name == "main") return AIMP_MPH_MAINFORM;
    if (name == "playlist") return AIMP_MPH_PLAYLISTFORM;
    if (name == "equalizer") return AIMP_MPH_EQUALIZERFORM;
    return -1;
}
HRESULT GetWindow(IAIMPServiceMessageDispatcher* dispatcher, int id, HWND& window) {
    window = nullptr;
    HRESULT hr = dispatcher->Send(AIMP_MSG_PROPERTY_HWND, id, &window);
    DWORD process = 0;
    if (hr != S_OK || !window || !IsWindow(window)) return hr == S_OK ? E_HANDLE : hr;
    GetWindowThreadProcessId(window, &process);
    return process == GetCurrentProcessId() ? S_OK : E_ACCESSDENIED;
}
}

json GetAppControlCapabilities() {
    Ref<IAIMPServiceMessageDispatcher> dispatcher; Service(IID_IAIMPServiceMessageDispatcher, dispatcher);
    Ref<IAIMPServiceActionManager> actions; Service(IID_IAIMPServiceActionManager, actions);
    json windows = json::array();
    if (dispatcher) for (const char* name : {"main", "playlist", "equalizer"}) {
        HWND window = nullptr;
        windows.push_back({{"id", name}, {"available", GetWindow(dispatcher.p, WindowId(name), window) == S_OK}});
    }
    return {{"app_control", {{"notifications", bool(dispatcher)}, {"windows", windows},
        {"action_lookup", bool(actions)}, {"action_execute", bool(actions)}, {"action_enumeration", false},
        {"quit", bool(dispatcher)}}}};
}

json GetAppControlState() {
    json state = {{"loaded", nullptr}, {"buffering", nullptr}, {"last_action", lastAction}};
    Ref<IAIMPServiceMessageDispatcher> dispatcher;
    if (Service(IID_IAIMPServiceMessageDispatcher, dispatcher) == S_OK && dispatcher) {
        BOOL loaded = FALSE; float buffering = 0;
        if (dispatcher->Send(AIMP_MSG_PROPERTY_LOADED, AIMP_MSG_PROPVALUE_GET, &loaded) == S_OK) state["loaded"] = loaded != FALSE;
        if (dispatcher->Send(AIMP_MSG_PROPERTY_PLAYER_BUFFERING, AIMP_MSG_PROPVALUE_GET, &buffering) == S_OK && std::isfinite(buffering) && buffering >= 0 && buffering <= 100) state["buffering"] = buffering;
    }
    return {{"app_control", state}};
}

bool HandleAppControlCommand(const std::string& action, const json& cmd, json& response) {
    if (action != "app/notification" && action != "app/notification/clear" && action != "app/window" &&
        action != "app/action/get" && action != "app/action/execute" && action != "app/quit" && action != "app/control/get") return false;
    if (!g_core || GetCurrentThreadId() != g_mainThreadId) { response = Error("MAIN_THREAD_REQUIRED"); return true; }
    if (action == "app/control/get") { response = GetAppControlState(); return true; }
    if (action == "app/action/get" || action == "app/action/execute") {
        if (!Text(cmd, "id", 512)) { response = Error("INVALID_ACTION_ID"); return true; }
        const auto id = cmd["id"].get<std::string>();
        Ref<IAIMPAction> found;
        HRESULT hr = Lookup(id, found);
        if (hr != S_OK || !found) { response = Error("ACTION_NOT_FOUND", hr == S_OK ? E_NOINTERFACE : hr); return true; }
        json info = ActionInfo(id, found.p);
        if (action == "app/action/get") { response = {{"app_action", info}}; return true; }
        if (info["enabled"] != true || info["executable"] != true) { response = Error("ACTION_UNAVAILABLE"); return true; }
        if (actionBusy) { response = Error("ACTION_BUSY"); return true; }
        if (!EnsureActionWindow()) { response = Error("ACTION_QUEUE_UNAVAILABLE"); return true; }
        pendingAction = id; actionBusy = true; lastAction = {{"id", id}, {"status", "queued"}};
        if (!PostMessageW(actionWindow, ActionMessage, 0, 0)) {
            actionBusy = false; pendingAction.clear(); response = Error("ACTION_QUEUE_FAILED", HRESULT_FROM_WIN32(GetLastError())); return true;
        }
        response = {{"acknowledgement", "queued"}, {"app_action", info}, {"last_action", lastAction}};
        return true;
    }
    Ref<IAIMPServiceMessageDispatcher> dispatcher;
    HRESULT hr = Service(IID_IAIMPServiceMessageDispatcher, dispatcher);
    if (hr != S_OK || !dispatcher) { response = Error("MESSAGE_DISPATCHER_UNAVAILABLE", hr); return true; }
    if (action == "app/notification" || action == "app/notification/clear") {
        if (action == "app/notification" && (!Text(cmd, "text", 4096) || (cmd.contains("persistent") && !cmd["persistent"].is_boolean()))) {
            response = Error("INVALID_NOTIFICATION"); return true;
        }
        const bool clear = action == "app/notification/clear";
        notificationText = clear ? L"" : Utf8ToWStr(cmd["text"].get<std::string>());
        if (!clear && notificationText.empty()) { response = Error("INVALID_NOTIFICATION"); return true; }
        hr = dispatcher->Send(AIMP_MSG_CMD_SHOW_NOTIFICATION, clear || cmd.value("persistent", false) ? 1 : 0,
            clear ? nullptr : notificationText.data());
        response = hr == S_OK ? json{{"ok", true}} : Error("NOTIFICATION_FAILED", hr);
        return true;
    }
    if (action == "app/quit") {
        if (!cmd.contains("confirm") || cmd["confirm"] != true) { response = Error("CONFIRM_REQUIRED"); return true; }
        HWND window = nullptr; hr = GetWindow(dispatcher.p, AIMP_MPH_APPLICATION, window);
        if (hr != S_OK) hr = GetWindow(dispatcher.p, AIMP_MPH_MAINFORM, window);
        // Defer shutdown on AIMP's window, so no plug-in code is running when it unloads.
        if (hr == S_OK && !PostMessageW(window, WM_CLOSE, 0, 0)) hr = HRESULT_FROM_WIN32(GetLastError());
        response = hr == S_OK ? json{{"acknowledgement", "queued"}} : Error("QUIT_FAILED", hr);
        return true;
    }
    if (!Text(cmd, "window", 20) || !Text(cmd, "operation", 20)) { response = Error("INVALID_WINDOW_OPERATION"); return true; }
    const auto name = cmd["window"].get<std::string>(), operation = cmd["operation"].get<std::string>();
    const int id = WindowId(name);
    if (id < 0) { response = Error("INVALID_WINDOW"); return true; }
    HWND window = nullptr; hr = GetWindow(dispatcher.p, id, window);
    if (hr != S_OK) { response = Error("WINDOW_UNAVAILABLE", hr); return true; }
    int show;
    if (operation == "show") show = SW_SHOW;
    else if (operation == "hide") show = SW_HIDE;
    else if (operation == "minimize") show = SW_MINIMIZE;
    else if (operation == "restore" || operation == "foreground") show = SW_RESTORE;
    else { response = Error("INVALID_WINDOW_OPERATION"); return true; }
    ShowWindowAsync(window, show);
    if (operation == "foreground" && !SetForegroundWindow(window)) { response = Error("FOREGROUND_DENIED", E_ACCESSDENIED); return true; }
    response = {{"acknowledgement", "queued"}, {"window", name}, {"operation", operation}};
    return true;
}

void FinalizeAppControls() {
    if (actionWindow) DestroyWindow(actionWindow);
    actionWindow = nullptr; actionBusy = false; pendingAction.clear(); lastAction = nullptr;
    UnregisterClassW(ActionWindowClass, g_hInstance);
    notificationText.clear();
}
