#include "utils.h"
#include <climits>
std::string WStrToUtf8(const std::wstring& w) {
    if (w.empty() || w.size() > INT_MAX) return {};
    int length = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    if (length <= 0) return {};
    std::string s(length, 0);
    if (!WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), length, nullptr, nullptr)) return {};
    return s;
}
std::string WStr(const wchar_t* w) { return w ? WStrToUtf8(w) : ""; }
std::wstring Utf8ToWStr(const std::string& s) {
    if (s.empty() || s.size() > INT_MAX) return {};
    int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), int(s.size()), nullptr, 0);
    if (length <= 0) return {};
    std::wstring w(length, 0);
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), int(s.size()), w.data(), length)) return {};
    return w;
}
