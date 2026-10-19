#include "protocol.h"
#include "network.h"
#include "globals.h"

std::vector<NetworkInterface> EnumNetworkInterfaces() {
    std::vector<NetworkInterface> result;
    ULONG bufLen = 15000;
    std::vector<BYTE> buf(bufLen);
    for (int attempt = 0; attempt < 3; ++attempt) {
        ULONG ret = GetAdaptersAddresses(AF_INET,
            GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
            nullptr, (IP_ADAPTER_ADDRESSES*)buf.data(), &bufLen);
        if (ret == ERROR_BUFFER_OVERFLOW) {
            buf.resize(bufLen);
            continue;
        }
        if (ret != ERROR_SUCCESS) break;
        for (auto* a = (IP_ADAPTER_ADDRESSES*)buf.data(); a; a = a->Next) {
            if (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
            if (a->OperStatus != IfOperStatusUp) continue;
            for (auto* ua = a->FirstUnicastAddress; ua; ua = ua->Next) {
                auto* sin = (sockaddr_in*)ua->Address.lpSockaddr;
                wchar_t ipBuf[INET_ADDRSTRLEN] = {};
                InetNtopW(AF_INET, &sin->sin_addr, ipBuf, INET_ADDRSTRLEN);
                std::wstring friendly = a->FriendlyName ? std::wstring(a->FriendlyName) : L"Unknown";
                NetworkInterface iface;
                iface.ip          = ipBuf;
                iface.displayName = friendly + L" (" + ipBuf + L")";
                result.push_back(std::move(iface));
            }
        }
        break;
    }
    return result;
}

bool IsAllowedClient(uint32_t clientIpNetworkOrder) {
    return !g_allowListEnabled || protocol::Allowed(ntohl(clientIpNetworkOrder), g_allowList);
}
