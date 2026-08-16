#include "net/socket.h"

// Winsock must come before <windows.h>, which ws2tcpip.h pulls in.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>

#include <algorithm>
#include <cstring>

namespace ww::net {
namespace {

bool g_started = false;
thread_local std::string g_error;

// Winsock error codes are not worth showing a player. Translate the handful
// that actually happen in this flow and fall back to the number otherwise.
void set_error(const std::string& what, int code) {
    const char* why = nullptr;
    switch (code) {
    case WSAECONNREFUSED: why = "connection refused (is the host waiting, and the port open?)"; break;
    case WSAETIMEDOUT:    why = "timed out (no reply from that address)"; break;
    case WSAEHOSTUNREACH: why = "host unreachable"; break;
    case WSAENETUNREACH:  why = "network unreachable"; break;
    case WSAECONNRESET:   why = "connection reset by the other player"; break;
    case WSAEADDRINUSE:   why = "that port is already in use on this machine"; break;
    case WSAEACCES:       why = "permission denied (blocked by a firewall?)"; break;
    default: break;
    }
    g_error = what;
    g_error += ": ";
    if (why) {
        g_error += why;
    } else {
        g_error += "error ";
        g_error += std::to_string(code);
    }
}

SOCKET sock(Handle h) { return static_cast<SOCKET>(h); }

void set_nonblocking(SOCKET s) {
    u_long mode = 1;
    ioctlsocket(s, FIONBIO, &mode);
}

} // namespace

bool startup() {
    if (g_started) return true;
    WSADATA wsa;
    int rc = WSAStartup(MAKEWORD(2, 2), &wsa);
    if (rc != 0) {
        set_error("network startup", rc);
        return false;
    }
    g_started = true;
    return true;
}

void shutdown_all() {
    if (!g_started) return;
    WSACleanup();
    g_started = false;
}

const std::string& last_error() { return g_error; }

Handle tcp_listen(uint16_t port) {
    if (!startup()) return kInvalid;
    SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        set_error("create socket", WSAGetLastError());
        return kInvalid;
    }
    // Without SO_REUSEADDR, hosting again within the TIME_WAIT window after a
    // previous match fails with "address in use" for a couple of minutes --
    // which reads to the player as the game being broken.
    BOOL yes = TRUE;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof(yes));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);
    if (::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        set_error("bind port " + std::to_string(port), WSAGetLastError());
        closesocket(s);
        return kInvalid;
    }
    if (::listen(s, 1) == SOCKET_ERROR) {
        set_error("listen", WSAGetLastError());
        closesocket(s);
        return kInvalid;
    }
    set_nonblocking(s);
    return static_cast<Handle>(s);
}

Handle tcp_accept(Handle listener) {
    if (listener == kInvalid) return kInvalid;
    SOCKET c = ::accept(sock(listener), nullptr, nullptr);
    if (c == INVALID_SOCKET) return kInvalid; // WSAEWOULDBLOCK is the normal case
    set_nonblocking(c);
    return static_cast<Handle>(c);
}

Handle tcp_connect(const std::string& host, uint16_t port) {
    if (!startup()) return kInvalid;
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    std::string port_s = std::to_string(port);
    if (getaddrinfo(host.c_str(), port_s.c_str(), &hints, &res) != 0 || !res) {
        g_error = "could not resolve '" + host + "'";
        return kInvalid;
    }
    SOCKET s = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (s == INVALID_SOCKET) {
        set_error("create socket", WSAGetLastError());
        freeaddrinfo(res);
        return kInvalid;
    }
    set_nonblocking(s);
    ::connect(s, res->ai_addr, static_cast<int>(res->ai_addrlen)); // in progress
    freeaddrinfo(res);
    return static_cast<Handle>(s);
}

int tcp_connected(Handle s) {
    if (s == kInvalid) return -1;
    fd_set wr, ex;
    FD_ZERO(&wr);
    FD_ZERO(&ex);
    FD_SET(sock(s), &wr);
    FD_SET(sock(s), &ex);
    timeval tv{0, 0};
    int n = select(0, nullptr, &wr, &ex, &tv);
    if (n == SOCKET_ERROR) {
        set_error("connect", WSAGetLastError());
        return -1;
    }
    if (FD_ISSET(sock(s), &ex)) {
        int err = 0;
        int len = sizeof(err);
        getsockopt(sock(s), SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &len);
        set_error("connect", err ? err : WSAECONNREFUSED);
        return -1;
    }
    if (FD_ISSET(sock(s), &wr)) {
        // Writable can still mean "failed" on some stacks -- confirm via SO_ERROR.
        int err = 0;
        int len = sizeof(err);
        getsockopt(sock(s), SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &len);
        if (err != 0) {
            set_error("connect", err);
            return -1;
        }
        return 1;
    }
    return 0;
}

bool tcp_send(Handle s, const uint8_t* data, size_t n) {
    if (s == kInvalid) return false;
    size_t sent = 0;
    while (sent < n) {
        int r = ::send(sock(s), reinterpret_cast<const char*>(data + sent),
                       static_cast<int>(n - sent), 0);
        if (r == SOCKET_ERROR) {
            int err = WSAGetLastError();
            if (err == WSAEWOULDBLOCK) continue; // send buffer full; turn packets are tiny
            set_error("send", err);
            return false;
        }
        sent += static_cast<size_t>(r);
    }
    return true;
}

bool tcp_recv(Handle s, std::vector<uint8_t>& out) {
    if (s == kInvalid) return false;
    char buf[4096];
    for (;;) {
        int r = ::recv(sock(s), buf, sizeof(buf), 0);
        if (r > 0) {
            out.insert(out.end(), buf, buf + r);
            continue; // drain everything available this frame
        }
        if (r == 0) {
            g_error = "the other player disconnected";
            return false; // orderly close
        }
        int err = WSAGetLastError();
        if (err == WSAEWOULDBLOCK) return true; // nothing more right now
        set_error("receive", err);
        return false;
    }
}

void close_socket(Handle s) {
    if (s != kInvalid) closesocket(sock(s));
}

void set_nodelay(Handle s) {
    BOOL yes = TRUE;
    setsockopt(sock(s), IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&yes), sizeof(yes));
}

Handle udp_socket(const std::string& bind_iface) {
    if (!startup()) return kInvalid;
    SOCKET s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) {
        set_error("create udp socket", WSAGetLastError());
        return kInvalid;
    }
    if (!bind_iface.empty()) {
        BOOL yes = TRUE;
        setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof(yes));
        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_port = 0; // any ephemeral port; the router replies to it
        if (inet_pton(AF_INET, bind_iface.c_str(), &local.sin_addr) != 1 ||
            ::bind(s, reinterpret_cast<sockaddr*>(&local), sizeof(local)) == SOCKET_ERROR) {
            set_error("bind udp to " + bind_iface, WSAGetLastError());
            closesocket(s);
            return kInvalid;
        }
        // Pin OUTGOING multicast to this interface too. Binding the socket sets
        // the source address but does not, on Windows, decide which adapter a
        // multicast datagram leaves by -- that is IP_MULTICAST_IF's job, and
        // without it the M-SEARCH can still take the wrong route.
        setsockopt(s, IPPROTO_IP, IP_MULTICAST_IF,
                   reinterpret_cast<const char*>(&local.sin_addr), sizeof(local.sin_addr));
        DWORD ttl = 4; // enough to cross a router or two; SSDP convention
        setsockopt(s, IPPROTO_IP, IP_MULTICAST_TTL, reinterpret_cast<const char*>(&ttl), sizeof(ttl));
    }
    return static_cast<Handle>(s);
}

std::vector<std::string> local_addresses() {
    std::vector<std::string> out;
    if (!startup()) return out;
    char host[256] = {};
    if (gethostname(host, sizeof(host)) == SOCKET_ERROR) return out;
    addrinfo hints{};
    hints.ai_family = AF_INET;
    addrinfo* res = nullptr;
    if (getaddrinfo(host, nullptr, &hints, &res) != 0) return out;
    for (addrinfo* p = res; p; p = p->ai_next) {
        auto* a = reinterpret_cast<sockaddr_in*>(p->ai_addr);
        char txt[INET_ADDRSTRLEN] = {};
        inet_ntop(AF_INET, &a->sin_addr, txt, sizeof(txt));
        std::string s = txt;
        if (s == "127.0.0.1") continue;
        if (std::find(out.begin(), out.end(), s) == out.end()) out.push_back(s);
    }
    freeaddrinfo(res);
    // Put the default-route address first: it is the most likely to face the
    // router, so discovery usually succeeds on the first attempt.
    std::string primary = local_address();
    auto at = std::find(out.begin(), out.end(), primary);
    if (at != out.end() && at != out.begin()) std::iter_swap(out.begin(), at);
    else if (at == out.end() && !primary.empty()) out.insert(out.begin(), primary);
    return out;
}

bool udp_send_to(Handle s, const std::string& host, uint16_t port, const std::string& payload) {
    if (s == kInvalid) return false;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, host.c_str(), &addr.sin_addr);
    int r = ::sendto(sock(s), payload.data(), static_cast<int>(payload.size()), 0,
                     reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    if (r == SOCKET_ERROR) {
        set_error("udp send", WSAGetLastError());
        return false;
    }
    return true;
}

std::string udp_recv(Handle s, int timeout_ms) {
    if (s == kInvalid) return {};
    fd_set rd;
    FD_ZERO(&rd);
    FD_SET(sock(s), &rd);
    timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    if (select(0, &rd, nullptr, nullptr, &tv) <= 0) return {};
    char buf[4096];
    int r = ::recvfrom(sock(s), buf, sizeof(buf), 0, nullptr, nullptr);
    if (r <= 0) return {};
    return std::string(buf, buf + r);
}

std::string local_address() {
    if (!startup()) return {};
    // The address that would be used to reach the internet -- i.e. the one on
    // the interface with the default route. Connecting a UDP socket sends no
    // packets, it just asks the routing table, which is exactly the question.
    // Enumerating adapters instead picks up VPN/virtual/loopback interfaces and
    // routinely returns the wrong one.
    SOCKET s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return {};
    sockaddr_in probe{};
    probe.sin_family = AF_INET;
    probe.sin_port = htons(53);
    inet_pton(AF_INET, "8.8.8.8", &probe.sin_addr);
    std::string out;
    if (::connect(s, reinterpret_cast<sockaddr*>(&probe), sizeof(probe)) != SOCKET_ERROR) {
        sockaddr_in me{};
        int len = sizeof(me);
        if (getsockname(s, reinterpret_cast<sockaddr*>(&me), &len) != SOCKET_ERROR) {
            char txt[INET_ADDRSTRLEN] = {};
            inet_ntop(AF_INET, &me.sin_addr, txt, sizeof(txt));
            out = txt;
        }
    }
    closesocket(s);
    return out;
}

} // namespace ww::net
