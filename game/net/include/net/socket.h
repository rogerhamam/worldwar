#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Thin, blocking-optional TCP/UDP wrapper over Winsock. Deliberately small:
// the lockstep protocol needs exactly one accepted connection, length-prefixed
// messages, and non-blocking reads that never stall the render loop. Anything
// more general would be code with no caller.
//
// Windows-only, like the game (win32 build, MinGW UCRT). The API is kept free
// of Winsock types so nothing outside socket.cpp has to include <winsock2.h> --
// that header is order-sensitive with <windows.h> and pulls in macros (near/
// far, min/max) that break other translation units.
namespace ww::net {

// Call once before anything else (WSAStartup); safe to call repeatedly.
// Returns false if the platform's socket stack could not be initialised, in
// which case every call below fails cleanly rather than crashing.
bool startup();
void shutdown_all();

// Last human-readable error from this thread's most recent failed call --
// surfaced straight to the player in the lobby, so it must stay readable
// ("connection refused", not "WSAECONNREFUSED 10061").
const std::string& last_error();

// An opaque socket handle. 0 is "not a socket"; the real value is an OS
// descriptor cast to uintptr_t so this header stays Winsock-free.
using Handle = uintptr_t;
constexpr Handle kInvalid = static_cast<Handle>(~0ull);

// ---- TCP ----------------------------------------------------------------
// Listen on `port` (all interfaces). Non-blocking.
Handle tcp_listen(uint16_t port);
// Accept a pending connection, or kInvalid if none is waiting. Never blocks.
Handle tcp_accept(Handle listener);
// Connect to host:port. Non-blocking: returns a handle immediately and
// tcp_connected() reports when the handshake finished (or failed).
Handle tcp_connect(const std::string& host, uint16_t port);
// 1 = connected, 0 = still connecting, -1 = failed.
int tcp_connected(Handle s);
// Send all of `data`. Returns false if the peer is gone. Buffers internally
// are the OS's; a short write is retried, so this either sends everything or
// reports failure.
bool tcp_send(Handle s, const uint8_t* data, size_t n);
// Read whatever is available into `out` (appended). Returns false if the peer
// closed or errored. Empty read with true == nothing available right now.
bool tcp_recv(Handle s, std::vector<uint8_t>& out);
void close_socket(Handle s);
// Disable Nagle. Lockstep sends one small packet per turn and needs it to
// leave immediately -- with Nagle on, a 20-byte turn packet can sit in the
// kernel for tens of milliseconds waiting for company, which shows up as the
// whole game stuttering at the turn rate.
void set_nodelay(Handle s);

// ---- UDP (used by UPnP's SSDP discovery) --------------------------------
// `bind_iface`, when non-empty, binds the socket to that local address. SSDP
// needs this: an unbound UDP socket sends multicast out whichever interface
// the routing table happens to prefer, and on a machine with a VPN, a virtual
// switch (WSL/Hyper-V/Docker all install one) or two NICs, that is regularly
// not the one the router is on -- so the M-SEARCH goes nowhere and discovery
// reports "no router" on a network that has a perfectly good one.
Handle udp_socket(const std::string& bind_iface = std::string());
// Every local IPv4 address on this machine, so SSDP can be attempted on each
// interface rather than guessing which one faces the router.
std::vector<std::string> local_addresses();
bool udp_send_to(Handle s, const std::string& host, uint16_t port, const std::string& payload);
// Receive with a timeout; returns empty if nothing arrived in time.
std::string udp_recv(Handle s, int timeout_ms);

// ---- addresses ----------------------------------------------------------
// This machine's LAN address (192.168.x.x / 10.x.x.x), used both to show the
// player their LAN address and as the internal client for the UPnP mapping.
std::string local_address();

} // namespace ww::net
