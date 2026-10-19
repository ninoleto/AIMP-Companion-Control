#!/usr/bin/env python3
"""Exercise the actual ManagePreset implementation against a portable SDK double.

This verifies validation, no-overwrite decisions, SDK failures, and readback.
It does not validate AIMP's COM implementation or Windows calling conventions.
"""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / "src/audio_controls.cpp").read_text()
body = source[source.index("json ManagePreset("):source.index("\nbool KnownAction(")]
prefix = r'''
#include "audio_catalog.h"
#include <cassert>
#include <cwctype>
#include <iostream>
#include <memory>
#include <vector>
using json = nlohmann::json;
using namespace audio_control;
using HRESULT = int;
constexpr HRESULT S_OK = 0, E_UNEXPECTED = -1, E_FAIL = -2;
constexpr int TRUE = 1, IID_IAIMPString = 1, IID_IAIMPEqualizerPreset = 2, IID_IAIMPServicePlayerEqualizerPresets = 3;
struct RefCount {
    int refs = 1;
    void AddRef() { ++refs; }
    void Release() { assert(refs > 0); --refs; }
};
struct IAIMPString : RefCount {
    std::wstring value;
    HRESULT SetData(wchar_t* text, int count) { value.assign(text, count); return S_OK; }
    wchar_t* GetData() { return value.data(); }
    HRESULT Compare(IAIMPString* other, int* result, int) {
        auto a = value, b = other->value;
        for (auto& c : a) c = std::towupper(c);
        for (auto& c : b) c = std::towupper(c);
        *result = a.compare(b); return S_OK;
    }
};
std::vector<std::unique_ptr<IAIMPString>> strings;
IAIMPString* String(std::wstring value = {}) {
    auto p = std::make_unique<IAIMPString>(); p->value = value;
    auto result = p.get(); strings.push_back(std::move(p)); return result;
}
bool renameFails = false, deleteFails = false, renameStale = false, deleteStale = false;
int writes = 0, enumFailure = -1;
struct IAIMPEqualizerPreset : RefCount {
    std::wstring value;
    HRESULT GetName(IAIMPString** target) { *target = String(value); return S_OK; }
    HRESULT SetName(IAIMPString* name) {
        ++writes; if (renameFails) return E_FAIL;
        if (!renameStale) value = name->value; return S_OK;
    }
};
std::vector<std::unique_ptr<IAIMPEqualizerPreset>> presetsOwned;
struct IAIMPServicePlayerEqualizerPresets : RefCount {
    std::vector<IAIMPEqualizerPreset*> items;
    int GetPresetCount() { return static_cast<int>(items.size()); }
    HRESULT GetPreset(int index, int, void** target) {
        if (index == enumFailure) return E_FAIL;
        items.at(index)->AddRef(); *target = items.at(index); return S_OK;
    }
    HRESULT Delete(IAIMPEqualizerPreset* item) {
        ++writes; if (deleteFails) return E_FAIL;
        if (!deleteStale) items.erase(std::find(items.begin(), items.end(), item)); return S_OK;
    }
} library;
struct Core { HRESULT CreateObject(int, void** target) { *target = String(); return S_OK; } } core;
Core* g_core = &core;
template<class T> class Ref {
    T* value_ = nullptr;
public:
    Ref() = default;
    Ref(const Ref&) = delete;
    ~Ref() { if (value_) value_->Release(); }
    T* get() const { return value_; }
    T* operator->() const { return value_; }
    explicit operator bool() const { return value_ != nullptr; }
    T** out() { if (value_) value_->Release(); value_ = nullptr; return &value_; }
    void** outVoid() { return reinterpret_cast<void**>(out()); }
};
bool Service(int, Ref<IAIMPServicePlayerEqualizerPresets>& target) { library.AddRef(); *target.out() = &library; return true; }
std::string WStr(const wchar_t* text) { std::wstring value(text); return std::string(value.begin(), value.end()); }
std::wstring Utf8ToWStr(const std::string& text) { return std::wstring(text.begin(), text.end()); }
json Failure(const char* code, HRESULT hr = S_OK) {
    json result = {{"error", {{"code", code}}}};
    if (hr != S_OK) result["error"]["hresult"] = hr; return result;
}
json Snapshot() {
    json names = json::array();
    for (const auto* item : library.items) names.push_back(WStr(item->value.c_str()));
    return {{"equalizer", {{"presets", names}}}};
}
void Reset(std::initializer_list<std::wstring> names = {L"Rock", L"Speech"}) {
    library.items.clear(); presetsOwned.clear(); strings.clear();
    renameFails = deleteFails = renameStale = deleteStale = false;
    writes = 0; enumFailure = -1;
    for (const auto& name : names) {
        auto p = std::make_unique<IAIMPEqualizerPreset>(); p->value = name;
        library.items.push_back(p.get()); presetsOwned.push_back(std::move(p));
    }
}
void Error(const json& value, const char* code) { assert(value.contains("error") && value["error"]["code"] == code); }
'''
suffix = r'''
int main() {
    Reset(); Error(ManagePreset({{"name", "Rock"}}, false), "CONFIRMATION_REQUIRED"); assert(writes == 0);
    Error(ManagePreset({{"name", "Rock"}, {"confirm", 1}}, false), "CONFIRMATION_REQUIRED"); assert(writes == 0);
    Error(ManagePreset({{"name", "Rock"}, {"confirm", false}}, false), "CONFIRMATION_REQUIRED"); assert(writes == 0);
    Error(ManagePreset({{"name", "missing"}, {"confirm", true}}, false), "EQUALIZER_PRESET_NOT_FOUND"); assert(writes == 0);
    Error(ManagePreset({{"name", "rock"}, {"confirm", true}}, false), "EQUALIZER_PRESET_NOT_FOUND"); assert(writes == 0);
    Error(ManagePreset({{"name", "Rock"}, {"new_name", "../bad"}, {"confirm", true}}, true), "INVALID_EQUALIZER_PRESET_NAME"); assert(writes == 0);
    Error(ManagePreset({{"name", "Rock"}, {"new_name", "speech"}, {"confirm", true}}, true), "EQUALIZER_PRESET_ALREADY_EXISTS"); assert(writes == 0);
    auto same = ManagePreset({{"name", "Rock"}, {"new_name", "Rock"}, {"confirm", true}}, true);
    assert(!same.contains("error") && same["renamed_preset"]["changed"] == false && writes == 0);
    enumFailure = 1; Error(ManagePreset({{"name", "Rock"}, {"new_name", "New"}, {"confirm", true}}, true), "EQUALIZER_PRESET_ENUMERATION_FAILED"); assert(writes == 0);
    Reset({L"Rock", L"Rock"}); Error(ManagePreset({{"name", "Rock"}, {"confirm", true}}, false), "AMBIGUOUS_EQUALIZER_PRESET_NAME"); assert(writes == 0);
    Reset(); auto renamed = ManagePreset({{"name", "Rock"}, {"new_name", "ROCK"}, {"confirm", true}}, true);
    assert(!renamed.contains("error") && writes == 1 && library.items[0]->value == L"ROCK");
    Reset(); renameFails = true; Error(ManagePreset({{"name", "Rock"}, {"new_name", "New"}, {"confirm", true}}, true), "EQUALIZER_PRESET_RENAME_FAILED"); assert(library.items[0]->value == L"Rock");
    Reset(); renameStale = true; auto stale = ManagePreset({{"name", "Rock"}, {"new_name", "New"}, {"confirm", true}}, true);
    Error(stale, "EQUALIZER_PRESET_READBACK_FAILED"); assert(stale["mutation_applied"] == true);
    Reset(); deleteFails = true; Error(ManagePreset({{"name", "Rock"}, {"confirm", true}}, false), "EQUALIZER_PRESET_DELETE_FAILED"); assert(library.items.size() == 2);
    Reset(); deleteStale = true; Error(ManagePreset({{"name", "Rock"}, {"confirm", true}}, false), "EQUALIZER_PRESET_READBACK_FAILED");
    Reset(); auto deleted = ManagePreset({{"name", "Rock"}, {"confirm", true}}, false);
    assert(!deleted.contains("error") && writes == 1 && library.items.size() == 1 && library.items[0]->value == L"Speech");
    assert(library.refs == 1);
    std::cout << "Preset-management validation: 16 scenarios passed\n";
}
'''
with tempfile.TemporaryDirectory(prefix="aimp-preset-test-") as folder:
    harness = Path(folder) / "preset.cpp"
    executable = Path(folder) / "preset-tests"
    harness.write_text(prefix + body + suffix)
    subprocess.run([os.environ.get("CXX", "g++"), "-std=c++17", "-O1", "-g", "-Wall", "-Wextra",
        "-Wno-misleading-indentation", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
        "-I" + str(root / "src"), "-isystem", str(root / "third_party"), str(harness), "-o", str(executable)], check=True)
    # LeakSanitizer cannot inspect /proc in this executor; ASan/UBSan remain on.
    subprocess.run([str(executable)], check=True, env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0"})
