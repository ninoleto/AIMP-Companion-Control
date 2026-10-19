#pragma once
#include "pch.h"



class CompanionOptionsFrame : public IAIMPOptionsDialogFrame {
    LONG ref_ = 1;
    IAIMPServiceOptionsDialog* svcOpts_ = nullptr;
    IAIMPOptionsDialogFrame* owner_ = nullptr; // Owner outlives this frame; no reference cycle.
public:
    explicit CompanionOptionsFrame(IAIMPServiceOptionsDialog* svc, IAIMPOptionsDialogFrame* owner);
    virtual ~CompanionOptionsFrame();

    // IUnknown
    HRESULT WINAPI QueryInterface(REFIID riid, void** ppv) override;
    ULONG   WINAPI AddRef()  override;
    ULONG   WINAPI Release() override;

    // IAIMPOptionsDialogFrame
    HRESULT WINAPI GetName(IAIMPString** s) override;
    HWND    WINAPI CreateFrame(HWND parentWnd) override;
    void    WINAPI DestroyFrame() override;
    void    WINAPI Notification(int id) override;
};
