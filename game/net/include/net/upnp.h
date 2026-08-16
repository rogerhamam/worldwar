#pragma once
#include <cstdint>
#include <string>

// UPnP IGD port mapping, so hosting a game does not require the player to log
// into their router and set up a port forward by hand.
//
// Hand-rolled over the plain sockets in socket.h rather than using Windows'
// IUPnPNAT COM object or vendoring miniupnpc. Three reasons: the COM path drags
// COM initialisation and apartment-threading rules into the render thread for
// one call; it cannot report WHY it failed, which is the difference between a
// useful message and "multiplayer doesn't work"; and the protocol we need is
// three HTTP requests, which is less code than either dependency would cost.
//
// Everything here blocks, and discovery can take a couple of seconds on a slow
// router. Call it off the render thread (see NetLobby, which runs it once when
// the player clicks Host) or accept a visible hitch.
namespace ww::net {

struct PortMapResult {
    bool discovered = false;   // an IGD (router) answered SSDP at all
    bool mapped = false;       // the port mapping was actually created
    std::string external_ip;   // router's WAN address, empty if it wouldn't say
    std::string router_name;   // for the lobby status line
    std::string error;         // why it failed, in words a player can act on
};

// Discover the router and map `port` (TCP) to this machine. Idempotent-ish:
// routers accept a repeated identical mapping, and the description string is
// fixed so a stale mapping from a previous session is simply overwritten.
PortMapResult map_port(uint16_t port);

// Remove a mapping created by map_port. Best effort -- a router that has
// forgotten it (rebooted, lease expired) is not an error worth reporting.
void unmap_port(uint16_t port);

// The WAN address alone, without touching any mapping. Used to show the host
// the address to read out even when mapping failed (they may have forwarded
// the port manually, or be on a public IP already).
std::string external_ip();

} // namespace ww::net
