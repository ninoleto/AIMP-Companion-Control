#include "pch.h"
#include "globals.h"
#include "settings.h"
#include "focus_sync.h"
#include "ws_server.h"
#include "options_frame.h"
#include "player_extensions.h"
#include "media_controls.h"
#include "music_library_controls.h"
#include "app_controls.h"
#include "ui_lifetime.h"
#include <new>

class CompanionControlPlugin final : public IAIMPPlugin, public IAIMPOptionsDialogFrame {
    LONG refs_ = 1;
    CompanionOptionsFrame* frame_ = nullptr;
    bool registered_ = false;
public:
    HRESULT WINAPI QueryInterface(REFIID id, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (id == IID_IUnknown) *out = static_cast<IAIMPPlugin*>(this);
        else if (id == IID_IAIMPOptionsDialogFrame) *out = static_cast<IAIMPOptionsDialogFrame*>(this);
        else return E_NOINTERFACE;
        AddRef(); return S_OK;
    }
    ULONG WINAPI AddRef() override { return InterlockedIncrement(&refs_); }
    ULONG WINAPI Release() override { LONG n = InterlockedDecrement(&refs_); if (!n) delete this; return n; }
    TChar* WINAPI InfoGet(int index) override {
        static wchar_t name[] = L"AIMP Companion Control 0.3.1 preview";
        static wchar_t author[] = L"ninoleto; based on SLV Tech (MIT)";
        static wchar_t description[] = L"WebSocket control for Bitfocus Companion; local access by default";
        switch (index) {
        case AIMP_PLUGIN_INFO_NAME: return name;
        case AIMP_PLUGIN_INFO_AUTHOR: return author;
        case AIMP_PLUGIN_INFO_SHORT_DESCRIPTION: return description;
        default: return nullptr;
        }
    }
    LongWord WINAPI InfoGetCategories() override { return AIMP_PLUGIN_CATEGORY_ADDONS; }
    void WINAPI SystemNotification(int, IUnknown*) override {}
    HRESULT WINAPI Initialize(IAIMPCore* core) override {
        if (!core || g_core) return E_FAIL;
        IAIMPServicePlayer* player = nullptr;
        if (FAILED(core->QueryInterface(IID_IAIMPServicePlayer, (void**)&player)) || !player) return E_FAIL;
        player->Release();
        g_core = core; core->AddRef(); g_mainThreadId = GetCurrentThreadId();
        try {
            InitializeMediaControls();
            LoadSettings();
            IAIMPServiceOptionsDialog* options = nullptr;
            core->QueryInterface(IID_IAIMPServiceOptionsDialog, (void**)&options);
            frame_ = new CompanionOptionsFrame(options, static_cast<IAIMPOptionsDialogFrame*>(this));
            if (options) options->Release();
            registered_ = SUCCEEDED(core->RegisterExtension(IID_IAIMPServiceOptionsDialog, static_cast<IAIMPOptionsDialogFrame*>(this)));
            InitFocusSync();
            // Keep the settings page available on bind failure so the user can fix it.
            StartServer();
            return S_OK;
        } catch (...) { Finalize(); return E_FAIL; }
    }
    HRESULT WINAPI Finalize() override {
        ++uiLifecycleGeneration;
        StopServer();
        FinalizeMediaControls();
        FinalizeMusicLibraryControls();
        FinalizeAppControls();
        FinalizePlayerExtensions();
        FinalizeFocusSync();
        if (registered_ && g_core) { g_core->UnregisterExtension(static_cast<IAIMPOptionsDialogFrame*>(this)); registered_ = false; }
        if (frame_) { frame_->DestroyFrame(); delete frame_; frame_ = nullptr; }
        if (g_core) { g_core->Release(); g_core = nullptr; }
        g_mainThreadId = 0;
        return S_OK;
    }
    HRESULT WINAPI GetName(IAIMPString** out) override { return frame_ ? frame_->GetName(out) : E_FAIL; }
    HWND WINAPI CreateFrame(HWND parent) override { return frame_ ? frame_->CreateFrame(parent) : nullptr; }
    void WINAPI DestroyFrame() override { if (frame_) frame_->DestroyFrame(); }
    void WINAPI Notification(int id) override { if (frame_) frame_->Notification(id); }
};
extern "C" __declspec(dllexport) HRESULT WINAPI AIMPPluginGetHeader(IAIMPPlugin** header) {
    if (!header) return E_POINTER;
    *header = new (std::nothrow) CompanionControlPlugin();
    return *header ? S_OK : E_OUTOFMEMORY;
}
BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) g_hInstance = module;
    return TRUE;
}
