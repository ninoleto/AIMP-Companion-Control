#pragma once
#include "globals.h"
#include <cstdint>

// A modal AIMP command can re-enter plug-in Finalize from its settings dialog.
// Keep this DLL alive until the callback returns, including its COM destructors.
inline std::uint64_t uiLifecycleGeneration = 0;
class UiCallbackLease {
    HMODULE module_ = nullptr;
    std::uint64_t generation_ = uiLifecycleGeneration;
public:
    UiCallbackLease() {
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
            reinterpret_cast<LPCWSTR>(&uiLifecycleGeneration), &module_);
    }
    UiCallbackLease(const UiCallbackLease&) = delete;
    explicit operator bool() const { return module_ != nullptr; }
    bool current() const { return g_core && generation_ == uiLifecycleGeneration; }
    ~UiCallbackLease() {
        // If Finalize ran during this callback, the normal AIMP DLL reference
        // may already be gone. Keep the extra reference until process exit:
        // releasing it here could unmap the instructions that return to AIMP.
        if (module_ && current()) FreeLibrary(module_);
    }
};
