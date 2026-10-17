/*
 * jxs_discovery.cpp - the ONE boundary between the plugin (C11) and the
 * encoder's vendored DNS-SD parser (C++). See jxs_discovery.h.
 *
 * It calls bridge::discover() - the same translation units the bridge browser
 * and the MC5 obr-cam-browse helper use - and copies each service into a plain
 * C struct. It parses NO DNS itself.
 *
 * The AAAA trap: the camera publishes both an A record (its LAN IPv4) and an
 * AAAA record pointing at a global IPv6 that is NOT reachable from the LAN.
 * The parser merges A/AAAA into one address string, and if the AAAA won, the
 * advertised `address` would be IPv6. An IPv6 camera address is useless here
 * (the plugin's receive path and the node's HTTP API are IPv4), so when the
 * parsed address is not IPv4 we resolve the SRV host name with getaddrinfo()
 * bound to AF_INET - NEVER getaddrinfo()[0] unfiltered, which returns the
 * unreachable IPv6 first (the same AF_INET discipline jxs_controls.c uses).
 */
#include "jxs_discovery.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>

#include "bridge/discovery.h"
#include "bridge/mdns.h"

namespace
{

bool is_ipv4_text(const char* text)
{
  in_addr addr {};
  return text && text[0] != '\0' && ::inet_pton(AF_INET, text, &addr) == 1;
}

// An AF_INET-only resolution of a .local name, as a last resort when the
// parsed address is not IPv4. Returns empty when nothing resolves.
std::string resolve_ipv4(const std::string& host)
{
  if (host.empty())
  {
    return {};
  }
  addrinfo hints {};
  hints.ai_family = AF_INET;  // the AAAA trap: never accept IPv6 here
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* res = nullptr;
  if (::getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || !res)
  {
    return {};
  }
  char text[INET_ADDRSTRLEN] {};
  const auto* sin = reinterpret_cast<const sockaddr_in*>(res->ai_addr);
  const char* ok = ::inet_ntop(AF_INET, &sin->sin_addr, text, sizeof(text));
  std::string out = ok ? text : "";
  ::freeaddrinfo(res);
  return out;
}

void copy_txt(const bridge::mdns::service& svc,
              const char* key,
              const char* fallback,
              char* dst,
              std::size_t cap)
{
  const std::string value = svc.txt_value(key);
  const char* text = value.empty() ? (fallback ? fallback : "") : value.c_str();
  std::snprintf(dst, cap, "%s", text);
}

}  // namespace

extern "C" int jxs_discovery_scan(jxs_discovered_camera_t* out,
                                  int max,
                                  int window_ms)
{
  if (!out || max <= 0)
  {
    return 0;
  }
  if (window_ms < 200)
  {
    window_ms = 200;
  }
  if (window_ms > 8000)
  {
    window_ms = 8000;
  }

  const auto found = bridge::discover(std::chrono::milliseconds(window_ms),
                                      bridge::mdns::k_cam_service);

  int n = 0;
  for (const auto& svc : found)
  {
    if (n >= max)
    {
      break;
    }
    jxs_discovered_camera_t* d = &out[n];
    std::memset(d, 0, sizeof(*d));

    std::snprintf(d->instance, sizeof(d->instance), "%s", svc.instance.c_str());
    std::snprintf(d->host, sizeof(d->host), "%s", svc.host.c_str());
    d->service_port = svc.port;

    // The address to USE: the A record. Fall back to an AF_INET resolution of
    // the SRV host only when the parsed address is missing or (AAAA trap) IPv6.
    std::string address = svc.address;
    if (!is_ipv4_text(address.c_str()))
    {
      address = resolve_ipv4(svc.host);
    }
    std::snprintf(d->address, sizeof(d->address), "%s", address.c_str());

    copy_txt(svc, "name", svc.instance.c_str(), d->name, sizeof(d->name));
    copy_txt(svc, "codec", "", d->codec, sizeof(d->codec));
    copy_txt(svc, "resolution", "", d->resolution, sizeof(d->resolution));
    copy_txt(svc, "fps", "", d->fps, sizeof(d->fps));
    copy_txt(svc, "sampling", "", d->sampling, sizeof(d->sampling));
    copy_txt(svc, "api_port", "", d->api_port, sizeof(d->api_port));
    copy_txt(svc, "api_version", "", d->api_version, sizeof(d->api_version));

    ++n;
  }
  return n;
}
