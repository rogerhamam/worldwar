#include "net/upnp.h"

#include "net/socket.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

// UPnP IGD in three steps:
//   1. SSDP  -- UDP M-SEARCH to the multicast group; the router replies with a
//               LOCATION header pointing at its device description.
//   2. HTTP  -- GET that description, find the WAN connection service and its
//               control URL.
//   3. SOAP  -- POST AddPortMapping / DeletePortMapping / GetExternalIPAddress
//               to that control URL.
//
// The XML is picked apart with substring searches rather than a parser. That is
// a deliberate trade: the three values needed (LOCATION, controlURL, and one
// text node per SOAP response) sit in fixed, well-known element names that the
// IGD spec pins down, and pulling in an XML parser to read them would be far
// more code and far more attack surface than the tag scan below -- which never
// trusts a length, never allocates from a field in the document, and treats any
// malformed input as "no router found".
namespace ww::net {
namespace {

constexpr const char* kSsdpAddr = "239.255.255.250";
constexpr uint16_t kSsdpPort = 1900;
// Mapping description shown in the router's UI, and the key we match on when
// removing our own mapping later.
constexpr const char* kMapDesc = "World War";
// How long a created mapping lives if the game crashes without cleaning up.
// Routers commonly clamp or reject very long leases, and 0 ("permanent") is
// rejected outright by a good few of them, so this is a compromise: long
// enough for any match, short enough to self-clean.
constexpr const char* kLeaseSeconds = "7200";

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// Text between <tag> and </tag>, or empty. Case-sensitive: the IGD spec fixes
// the element names, and being lax here would risk matching the wrong element.
std::string tag_text(const std::string& xml, const std::string& tag) {
    std::string open = "<" + tag + ">", close = "</" + tag + ">";
    size_t a = xml.find(open);
    if (a == std::string::npos) return {};
    a += open.size();
    size_t b = xml.find(close, a);
    if (b == std::string::npos) return {};
    return xml.substr(a, b - a);
}

struct Url {
    std::string host;
    uint16_t port = 80;
    std::string path = "/";
};

bool parse_url(const std::string& url, Url& out) {
    const std::string prefix = "http://";
    if (url.compare(0, prefix.size(), prefix) != 0) return false;
    size_t start = prefix.size();
    size_t slash = url.find('/', start);
    std::string hostport = url.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
    out.path = slash == std::string::npos ? "/" : url.substr(slash);
    size_t colon = hostport.find(':');
    if (colon == std::string::npos) {
        out.host = hostport;
        out.port = 80;
    } else {
        out.host = hostport.substr(0, colon);
        int p = std::atoi(hostport.c_str() + colon + 1);
        if (p <= 0 || p > 65535) return false;
        out.port = static_cast<uint16_t>(p);
    }
    return !out.host.empty();
}

// Minimal blocking HTTP/1.1 request. Returns the BODY only, or empty on any
// failure. Routers are the only servers this ever talks to.
std::string http_request(const Url& url, const std::string& method, const std::string& extra_headers,
                         const std::string& body) {
    Handle s = tcp_connect(url.host, url.port);
    if (s == kInvalid) return {};
    // tcp_connect is non-blocking; wait briefly for it to settle.
    int state = 0;
    for (int i = 0; i < 300 && state == 0; ++i) { // ~3s
        state = tcp_connected(s);
        if (state == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    if (state != 1) {
        close_socket(s);
        return {};
    }
    std::string req = method + " " + url.path + " HTTP/1.1\r\n";
    req += "Host: " + url.host + ":" + std::to_string(url.port) + "\r\n";
    req += "Connection: close\r\n";
    req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    req += extra_headers;
    req += "\r\n";
    req += body;
    if (!tcp_send(s, reinterpret_cast<const uint8_t*>(req.data()), req.size())) {
        close_socket(s);
        return {};
    }
    std::vector<uint8_t> buf;
    // Read until the peer closes (Connection: close) or we give up. Routers
    // answer these in milliseconds; the cap stops a silent one hanging the
    // lobby.
    for (int i = 0; i < 400; ++i) { // ~4s
        size_t before = buf.size();
        if (!tcp_recv(s, buf)) break; // closed == done
        if (buf.size() == before) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        // A complete response with Connection: close ends at the close, but
        // stop early once we have a body and the router has gone quiet.
        if (buf.size() > 0 && i > 40 && buf.size() == before) break;
    }
    close_socket(s);
    std::string resp(buf.begin(), buf.end());
    size_t sep = resp.find("\r\n\r\n");
    if (sep == std::string::npos) return {};
    return resp.substr(sep + 4);
}

// SSDP M-SEARCH on ONE interface. Returns the LOCATION url of the first IGD
// that answers, or empty.
std::string discover_on(const std::string& iface) {
    Handle s = udp_socket(iface);
    if (s == kInvalid) return {};
    // Ask for both IGD versions and the generic root-device target. Routers
    // vary in which they answer: some only reply to their exact device type,
    // and a few answer nothing but ssdp:all / upnp:rootdevice.
    const char* targets[] = {"urn:schemas-upnp-org:device:InternetGatewayDevice:1",
                             "urn:schemas-upnp-org:device:InternetGatewayDevice:2",
                             "urn:schemas-upnp-org:service:WANIPConnection:1",
                             "upnp:rootdevice"};
    std::string location;
    for (const char* target : targets) {
        std::string msg = "M-SEARCH * HTTP/1.1\r\n";
        msg += "HOST: 239.255.255.250:1900\r\n";
        msg += "MAN: \"ssdp:discover\"\r\n";
        msg += "MX: 2\r\n";
        msg += "ST: ";
        msg += target;
        msg += "\r\n\r\n";
        // Multicast is lossy by design -- send each target twice, which is what
        // the SSDP spec itself recommends for exactly this reason.
        udp_send_to(s, kSsdpAddr, kSsdpPort, msg);
        udp_send_to(s, kSsdpAddr, kSsdpPort, msg);
        // Several devices may answer (printers, media servers); keep looking
        // until one names a LOCATION we can actually fetch.
        for (int i = 0; i < 10 && location.empty(); ++i) {
            std::string reply = udp_recv(s, 300);
            if (reply.empty()) continue;
            std::string low = lower(reply);
            size_t at = low.find("location:");
            if (at == std::string::npos) continue;
            at += 9;
            while (at < reply.size() && (reply[at] == ' ' || reply[at] == '\t')) ++at;
            size_t end = reply.find_first_of("\r\n", at);
            if (end == std::string::npos) continue;
            location = reply.substr(at, end - at);
        }
        if (!location.empty()) break;
    }
    close_socket(s);
    return location;
}

// Try every local interface. See udp_socket's bind_iface comment for why one
// unbound attempt is not enough on a typical Windows machine.
std::string discover_igd() {
    for (const std::string& iface : local_addresses()) {
        std::string loc = discover_on(iface);
        if (!loc.empty()) return loc;
    }
    return discover_on(std::string()); // last resort: let the OS choose
}

// The WAN service's control URL, resolved against the description's base.
struct Service {
    Url control;
    std::string type; // WANIPConnection:1 / WANPPPConnection:1 etc
    std::string router_name;
};

bool find_wan_service(const std::string& location, Service& out) {
    Url desc;
    if (!parse_url(location, desc)) return false;
    std::string xml = http_request(desc, "GET", "", "");
    if (xml.empty()) return false;
    out.router_name = tag_text(xml, "friendlyName");

    // Walk each <service> block and keep the first WAN connection service.
    // WANIPConnection is the common one; WANPPPConnection appears on
    // PPPoE-style gateways and speaks the identical action set.
    size_t pos = 0;
    while (true) {
        size_t a = xml.find("<service>", pos);
        if (a == std::string::npos) break;
        size_t b = xml.find("</service>", a);
        if (b == std::string::npos) break;
        std::string block = xml.substr(a, b - a);
        std::string type = tag_text(block, "serviceType");
        if (type.find("WANIPConnection") != std::string::npos ||
            type.find("WANPPPConnection") != std::string::npos) {
            std::string ctrl = tag_text(block, "controlURL");
            if (!ctrl.empty()) {
                out.type = type;
                if (ctrl.compare(0, 7, "http://") == 0) {
                    if (!parse_url(ctrl, out.control)) return false;
                } else {
                    out.control = desc; // same host/port as the description
                    out.control.path = ctrl[0] == '/' ? ctrl : "/" + ctrl;
                }
                return true;
            }
        }
        pos = b + 1;
    }
    return false;
}

std::string soap(const Service& svc, const std::string& action, const std::string& args) {
    std::string body =
        "<?xml version=\"1.0\"?>"
        "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
        "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
        "<s:Body><u:" + action + " xmlns:u=\"" + svc.type + "\">" + args +
        "</u:" + action + "></s:Body></s:Envelope>";
    std::string headers = "Content-Type: text/xml; charset=\"utf-8\"\r\n";
    headers += "SOAPAction: \"" + svc.type + "#" + action + "\"\r\n";
    return http_request(svc.control, "POST", headers, body);
}

} // namespace

PortMapResult map_port(uint16_t port) {
    PortMapResult r;
    startup();
    std::string location = discover_igd();
    if (location.empty()) {
        r.error = "no UPnP router found on this network";
        return r;
    }
    Service svc;
    if (!find_wan_service(location, svc)) {
        r.error = "router answered but exposes no port-mapping service";
        return r;
    }
    r.discovered = true;
    r.router_name = svc.router_name;

    std::string ip = local_address();
    if (ip.empty()) {
        r.error = "could not determine this machine's LAN address";
        return r;
    }

    std::string args =
        "<NewRemoteHost></NewRemoteHost>"
        "<NewExternalPort>" + std::to_string(port) + "</NewExternalPort>"
        "<NewProtocol>TCP</NewProtocol>"
        "<NewInternalPort>" + std::to_string(port) + "</NewInternalPort>"
        "<NewInternalClient>" + ip + "</NewInternalClient>"
        "<NewEnabled>1</NewEnabled>"
        "<NewPortMappingDescription>" + std::string(kMapDesc) + "</NewPortMappingDescription>"
        "<NewLeaseDuration>" + std::string(kLeaseSeconds) + "</NewLeaseDuration>";
    std::string resp = soap(svc, "AddPortMapping", args);
    if (resp.find("AddPortMappingResponse") != std::string::npos) {
        r.mapped = true;
    } else {
        // Some routers refuse a non-zero lease; retry as permanent before
        // giving up, since that is by far the commonest rejection.
        std::string permanent = args;
        size_t at = permanent.find("<NewLeaseDuration>");
        if (at != std::string::npos)
            permanent.replace(at, std::string::npos, "<NewLeaseDuration>0</NewLeaseDuration>");
        resp = soap(svc, "AddPortMapping", permanent);
        r.mapped = resp.find("AddPortMappingResponse") != std::string::npos;
        if (!r.mapped) {
            std::string code = tag_text(resp, "errorCode");
            r.error = "router refused the port mapping";
            if (!code.empty()) r.error += " (UPnP error " + code + ")";
            else if (resp.empty()) r.error = "router did not answer the mapping request";
        }
    }

    std::string ext = soap(svc, "GetExternalIPAddress", "");
    r.external_ip = tag_text(ext, "NewExternalIPAddress");
    return r;
}

void unmap_port(uint16_t port) {
    std::string location = discover_igd();
    if (location.empty()) return;
    Service svc;
    if (!find_wan_service(location, svc)) return;
    soap(svc,
         "DeletePortMapping",
         "<NewRemoteHost></NewRemoteHost>"
         "<NewExternalPort>" + std::to_string(port) + "</NewExternalPort>"
         "<NewProtocol>TCP</NewProtocol>");
}

std::string external_ip() {
    std::string location = discover_igd();
    if (location.empty()) return {};
    Service svc;
    if (!find_wan_service(location, svc)) return {};
    return tag_text(soap(svc, "GetExternalIPAddress", ""), "NewExternalIPAddress");
}

} // namespace ww::net
