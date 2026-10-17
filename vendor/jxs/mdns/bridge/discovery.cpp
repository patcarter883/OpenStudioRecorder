// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Pat Carter

#include <chrono>
#include <cstdint>
#include <cstring>
#include <vector>

#include "bridge/discovery.h"

#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
using socket_t = SOCKET;
#else
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#  include <sys/time.h>
#  include <unistd.h>
using socket_t = int;
#endif

namespace bridge
{
namespace
{
#ifdef _WIN32
// Winsock needs a process-wide start, and a matching cleanup per user.
struct winsock_guard
{
  winsock_guard()
  {
    WSADATA data;
    WSAStartup(MAKEWORD(2, 2), &data);
  }
  ~winsock_guard() { WSACleanup(); }
};

bool socket_valid(socket_t sock)
{
  return sock != INVALID_SOCKET;
}
void close_socket(socket_t sock)
{
  closesocket(sock);
}
#else
bool socket_valid(socket_t sock)
{
  return sock >= 0;
}
void close_socket(socket_t sock)
{
  ::close(sock);
}
#endif

const char* as_bytes(const void* value)
{
  return reinterpret_cast<const char*>(value);
}

// A receive buffer sized for a full mDNS response. Bridge advertisements are
// tiny; this is generous enough that a truncated read is not a concern.
constexpr int k_buffer_size = 4096;
}  // namespace

void merge_service(std::vector<mdns::service>& seen,
                   const mdns::service& incoming)
{
  if (incoming.instance.empty()) {
    return;
  }

  for (auto& existing : seen) {
    if (existing.instance != incoming.instance) {
      continue;
    }
    // Records arrive in any order: fill in only what is still unknown, and let
    // the most recent TXT win, because a bridge's own state is what it last
    // said.
    if (existing.host.empty()) {
      existing.host = incoming.host;
    }
    if (existing.address.empty()) {
      existing.address = incoming.address;
    }
    if (existing.port == 0) {
      existing.port = incoming.port;
    }
    for (const auto& [key, value] : incoming.txt) {
      existing.txt[key] = value;
    }
    return;
  }

  seen.push_back(incoming);
}

std::vector<mdns::service> discover(std::chrono::milliseconds window,
                                    const std::string& service_name)
{
  std::vector<mdns::service> found;

#ifdef _WIN32
  winsock_guard guard;
#endif

  const socket_t sock = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (!socket_valid(sock)) {
    return found;
  }

  const int yes = 1;
  // SO_REUSEADDR only, deliberately. Adding SO_REUSEPORT here would make the
  // kernel hand a multicast datagram to a SINGLE socket in the reuseport group,
  // so a system responder or a second instance of this app would silently
  // swallow every advertisement. With SO_REUSEADDR alone, every joined socket
  // receives.
  ::setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, as_bytes(&yes), sizeof(yes));

  // A browser binds the mDNS port and joins the group, because responders
  // answer to the multicast address rather than to the sender.
  sockaddr_in local {};
  local.sin_family = AF_INET;
  local.sin_addr.s_addr = htonl(INADDR_ANY);
  local.sin_port = htons(mdns::k_multicast_port);
  if (::bind(sock, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0) {
    close_socket(sock);
    return found;
  }

  ip_mreq group {};
  group.imr_multiaddr.s_addr = ::inet_addr(mdns::k_multicast_group);
  group.imr_interface.s_addr = htonl(INADDR_ANY);
  if (::setsockopt(
          sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, as_bytes(&group), sizeof(group))
      != 0)
  {
    close_socket(sock);
    return found;
  }

  // An advertisement is for this LAN: TTL 1 keeps it from being routed onward.
  const unsigned char ttl = 1;
  ::setsockopt(sock, IPPROTO_IP, IP_MULTICAST_TTL, as_bytes(&ttl), sizeof(ttl));

  // Ask, rather than waiting for an unsolicited announcement that may be
  // minutes away.
  const auto query = mdns::encode_query(service_name);
  sockaddr_in destination {};
  destination.sin_family = AF_INET;
  destination.sin_addr.s_addr = ::inet_addr(mdns::k_multicast_group);
  destination.sin_port = htons(mdns::k_multicast_port);
  ::sendto(sock,
           as_bytes(query.data()),
           static_cast<int>(query.size()),
           0,
           reinterpret_cast<sockaddr*>(&destination),
           sizeof(destination));

  const auto deadline = std::chrono::steady_clock::now() + window;
  for (;;) {
    const auto remaining =
        std::chrono::duration_cast<std::chrono::microseconds>(
            deadline - std::chrono::steady_clock::now());
    if (remaining.count() <= 0) {
      break;
    }

    timeval timeout {};
    timeout.tv_sec = static_cast<long>(remaining.count() / 1000000);
    timeout.tv_usec = static_cast<long>(remaining.count() % 1000000);
    ::setsockopt(
        sock, SOL_SOCKET, SO_RCVTIMEO, as_bytes(&timeout), sizeof(timeout));

    std::vector<std::uint8_t> buffer(k_buffer_size);
    const int received =
        ::recv(sock, reinterpret_cast<char*>(buffer.data()), k_buffer_size, 0);
    if (received <= 0) {
      break;  // the window elapsed, or the socket is done
    }

    buffer.resize(static_cast<std::size_t>(received));
    for (const auto& service : mdns::parse_response(buffer, service_name)) {
      merge_service(found, service);
    }
  }

  close_socket(sock);
  return found;
}

}  // namespace bridge
