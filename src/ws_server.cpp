#include "ws_server.h"
#include "globals.h"
#include "network.h"
#include "focus_sync.h"
#include "player_api.h"
#include "commands.h"
#include "protocol.h"
#include <bcrypt.h>
#include <atomic>
#include <memory>
#include <deque>

namespace {
constexpr wchar_t WindowClass[] = L"NinoAimpCompanionControl.Pump.v1";
constexpr size_t MaxClients = 4, MaxOutput = 4 * 1024 * 1024;
constexpr ULONGLONG TimeoutMs = 5000;
struct Client {
    SOCKET socket = INVALID_SOCKET;
    std::string input;
    std::deque<std::string> output;
    size_t outputBytes = 0, outputOffset = 0;
    protocol::Decoder decoder;
    bool ready = false, closing = false, dead = false;
    ULONGLONG accepted = GetTickCount64(), incomplete = 0, lastWrite = accepted;
    ULONGLONG allowanceAt = accepted;
    double allowance = 240;
    ~Client() { if (socket != INVALID_SOCKET) closesocket(socket); }
};
std::vector<std::unique_ptr<Client>> clients;
SOCKET listener = INVALID_SOCKET;
HWND window = nullptr;
bool wsaStarted = false, pumping = false;
std::atomic<unsigned> events{0};
ULONGLONG lastFocus = 0, lastPosition = 0;
std::wstring lastError;

void Queue(Client& c, std::string data) {
    if (c.dead || data.size() > MaxOutput - c.outputBytes) { c.dead = true; return; }
    if (c.output.empty()) c.lastWrite = GetTickCount64();
    c.outputBytes += data.size(); c.output.push_back(std::move(data));
}
void QueueJson(Client& c, const json& value) {
    Queue(c, protocol::Frame(value.dump(-1, ' ', false, json::error_handler_t::replace)));
}
void Close(Client& c, unsigned code) {
    if (c.closing) return;
    std::string payload; payload += char(code >> 8); payload += char(code);
    Queue(c, protocol::Frame(payload, 8)); c.closing = true;
}
void Flush(Client& c) {
    size_t budget = 65536;
    while (!c.dead && !c.output.empty() && budget) {
        auto& front = c.output.front();
        size_t remaining = std::min(front.size() - c.outputOffset, budget);
        int n = send(c.socket, front.data() + c.outputOffset, int(remaining), 0);
        if (n <= 0) {
            if (n < 0 && WSAGetLastError() == WSAEWOULDBLOCK) break;
            c.dead = true; break;
        }
        c.outputOffset += n; c.outputBytes -= n; budget -= n;
        c.lastWrite = GetTickCount64();
        if (c.outputOffset == front.size()) { c.output.pop_front(); c.outputOffset = 0; }
    }
    if (c.closing && c.output.empty()) c.dead = true;
    if (!c.output.empty() && GetTickCount64() - c.lastWrite > TimeoutMs) c.dead = true;
}
bool AcceptKey(const std::string& key, std::string& result) {
    // RFC 6455 requires SHA-1 for this non-secret handshake value. Use Windows CNG.
    std::string input = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    unsigned char hash[20];
    if (BCryptHash(BCRYPT_SHA1_ALG_HANDLE, nullptr, 0,
        reinterpret_cast<PUCHAR>(input.data()), ULONG(input.size()), hash, sizeof(hash)) < 0) return false;
    result = protocol::Base64(hash, sizeof(hash)); return true;
}
void Handshake(Client& c) {
    auto end = c.input.find("\r\n\r\n");
    if (end == std::string::npos) {
        if (c.input.size() > protocol::MaxHeader) c.dead = true;
        return;
    }
    auto checked = protocol::CheckHandshake(std::string_view(c.input).substr(0, end + 4));
    c.input.erase(0, end + 4);
    std::string accept;
    if (checked.status == 101 && !AcceptKey(checked.key, accept)) checked.status = 500;
    if (checked.status != 101) {
        Queue(c, "HTTP/1.1 " + std::to_string(checked.status) + " Rejected\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
        c.closing = true; return;
    }
    Queue(c, "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " + accept + "\r\n\r\n");
    c.ready = true;
    json status = ExecuteCommand({{"action", "player/status"}}); status["event"] = "connected";
    QueueJson(c, status);
}
void Receive(Client& c, int& commandBudget) {
    char buffer[8192];
    for (int i = 0; i < 2 && !c.closing && !c.dead; ++i) {
        int n = recv(c.socket, buffer, sizeof(buffer), 0);
        if (n == 0) { c.dead = true; return; }
        if (n < 0) {
            if (WSAGetLastError() != WSAEWOULDBLOCK) c.dead = true;
            break;
        }
        if (c.input.size() + size_t(n) > protocol::MaxMessage * 2 + 14) { c.dead = true; return; }
        c.input.append(buffer, n);
    }
    if (c.dead || c.closing) return;
    if (!c.ready) {
        Handshake(c);
        if (GetTickCount64() - c.accepted > TimeoutMs && !c.ready) c.dead = true;
        if (!c.ready || c.closing || c.dead) return;
    }
    for (int i = 0; i < 16 && commandBudget > 0 && !c.dead && !c.closing; ++i) {
        protocol::Message message;
        auto result = c.decoder.Next(c.input, message);
        if (result == protocol::Result::Error) { Close(c, c.decoder.closeCode); break; }
        if (result == protocol::Result::NeedMore) break;
        if (result == protocol::Result::Fragment) continue;
        if (message.opcode == 8) { Queue(c, protocol::Frame(message.payload, 8)); c.closing = true; break; }
        if (message.opcode == 9) { Queue(c, protocol::Frame(message.payload, 10)); continue; }
        if (message.opcode == 10) continue;
        --commandBudget;
        auto now = GetTickCount64();
        c.allowance = std::min(240.0, c.allowance + double(now - c.allowanceAt) * 0.12);
        c.allowanceAt = now;
        if (c.allowance < 1) { Close(c, 1008); break; }
        c.allowance -= 1;
        if (!protocol::ValidJsonDepth(message.payload)) { Close(c, 1007); break; }
        json cmd = json::parse(message.payload, nullptr, false);
        if (cmd.is_discarded()) { Close(c, 1007); break; }
        QueueJson(c, ExecuteCommand(cmd));
    }
    // A partially sent message has a total deadline, not a per-byte timeout.
    if (!c.input.empty() || c.decoder.fragmented()) {
        if (!c.incomplete) c.incomplete = GetTickCount64();
        else if (GetTickCount64() - c.incomplete > TimeoutMs) Close(c, 1008);
    } else c.incomplete = 0;
}
void Broadcast(json value, const char* event) {
    value["event"] = event;
    std::string frame = protocol::Frame(value.dump(-1, ' ', false, json::error_handler_t::replace));
    for (auto& c : clients) if (c->ready && !c->closing && !c->dead) Queue(*c, frame);
}
void Pump() {
    if (!g_running || pumping) return;
    pumping = true;
    struct Reset { ~Reset() { pumping = false; } } reset;
    // All sockets are nonblocking. Bound work per timer tick to protect the audio/UI thread.
    for (int i = 0; i < 4; ++i) {
        sockaddr_in address{}; int size = sizeof(address);
        SOCKET socket = accept(listener, reinterpret_cast<sockaddr*>(&address), &size);
        if (socket == INVALID_SOCKET) break;
        u_long nonblocking = 1;
        if (clients.size() >= MaxClients || !IsAllowedClient(address.sin_addr.s_addr) ||
            ioctlsocket(socket, FIONBIO, &nonblocking) != 0) { closesocket(socket); continue; }
        auto c = std::make_unique<Client>(); c->socket = socket;
        clients.push_back(std::move(c));
    }
    int commandBudget = 16;
    for (auto& c : clients) {
        try { Receive(*c, commandBudget); Flush(*c); }
        catch (...) { c->dead = true; }
    }
    clients.erase(std::remove_if(clients.begin(), clients.end(), [](const auto& c) { return c->dead; }), clients.end());
    if (!IsAimpLoaded()) return;
    auto now = GetTickCount64();
    if (now - lastFocus >= 100) { RefreshFocusFromPlayer(); lastFocus = now; }
    unsigned pending = events.exchange(0);
    if (clients.empty()) return;
    if (pending & 31) {
        json state = GetPlayerStatus();
        const char* name = (pending & NotifyTrack) ? "track_changed" :
            (pending & NotifyEnd) ? "playback_ended" : (pending & NotifyState) ? "player_state_changed" :
            (pending & NotifyFocus) ? "focus_changed" : "property_changed";
        Broadcast(state, name);
    }
    if (now - lastPosition >= 100 && (pending & NotifyPosition)) {
        IAIMPServicePlayer* player = nullptr;
        if (g_core && SUCCEEDED(g_core->QueryInterface(IID_IAIMPServicePlayer, (void**)&player)) && player) {
            double position = 0, duration = 0;
            player->GetPosition(&position); player->GetDuration(&duration); player->Release();
            Broadcast({{"position", position}, {"duration", duration}, {"remaining", std::max(0.0, duration - position)}}, "position");
        }
        lastPosition = now;
    }
    for (auto& c : clients) Flush(*c);
}
LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM w, LPARAM l) {
    if (message == WM_TIMER && w == 1) {
        try { Pump(); } catch (...) { /* Never propagate a C++ exception into AIMP/Win32. */ }
        return 0;
    }
    return DefWindowProcW(hwnd, message, w, l);
}
bool Fail(const wchar_t* description) {
    lastError = std::wstring(description) + L" (" + std::to_wstring(WSAGetLastError()) + L")";
    StopServer(); return false;
}
}
void WsNotify(unsigned flags) { events.fetch_or(flags, std::memory_order_relaxed); }
bool StartServer() {
    if (g_running) return true;
    if (GetCurrentThreadId() != g_mainThreadId) return false;
    lastError.clear();
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2,2), &data) != 0) { lastError = L"Winsock initialization failed"; return false; }
    wsaStarted = true;
    listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET) return Fail(L"Cannot create socket");
    BOOL exclusive = TRUE;
    if (setsockopt(listener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive)) != 0)
        return Fail(L"Cannot reserve port");
    sockaddr_in address{}; address.sin_family = AF_INET; address.sin_port = htons(u_short(g_port));
    if (g_bindMode == 0) address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    else if (g_bindMode == 1) address.sin_addr.s_addr = htonl(INADDR_ANY);
    else if (g_bindMode != 2 || g_bindIp.empty() || InetPtonW(AF_INET, g_bindIp.c_str(), &address.sin_addr) != 1)
        return Fail(L"Invalid listen address");
    if (g_allowListEnabled && !protocol::ValidAllowList(g_allowList)) return Fail(L"Invalid access list");
    u_long nonblocking = 1;
    if (ioctlsocket(listener, FIONBIO, &nonblocking) != 0) return Fail(L"Cannot set nonblocking mode");
    if (bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) return Fail(L"Cannot bind; check the address and port");
    if (listen(listener, 4) != 0) return Fail(L"Cannot listen");
    WNDCLASSW wc{}; wc.lpfnWndProc = WindowProc; wc.hInstance = g_hInstance; wc.lpszClassName = WindowClass;
    if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return Fail(L"Cannot create event class");
    window = CreateWindowExW(0, WindowClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, g_hInstance, nullptr);
    if (!window || !SetTimer(window, 1, 25, nullptr)) return Fail(L"Cannot start event timer");
    events.store(0); g_running = true; lastFocus = 0; lastPosition = 0;
    return true;
}
void StopServer() {
    g_running = false;
    if (window) { KillTimer(window, 1); DestroyWindow(window); window = nullptr; }
    clients.clear();
    if (listener != INVALID_SOCKET) { closesocket(listener); listener = INVALID_SOCKET; }
    if (wsaStarted) { WSACleanup(); wsaStarted = false; }
    UnregisterClassW(WindowClass, g_hInstance);
    events.store(0);
}
void RestartServer() { StopServer(); StartServer(); }
std::wstring ServerStatusText() {
    if (!g_running) return L"Stopped: " + (lastError.empty() ? std::wstring(L"server is not running") : lastError);
    std::wstring address = g_bindMode == 0 ? L"127.0.0.1" : g_bindMode == 2 ? g_bindIp : L"0.0.0.0";
    return L"WebSocket listening: " + address + L":" + std::to_wstring(g_port);
}
