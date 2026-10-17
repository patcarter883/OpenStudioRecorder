// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Pat Carter

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "bridge/mdns.h"

namespace bridge::mdns
{
namespace
{
constexpr std::size_t k_header_size = 12;
// A name can only be compressed so far before the packet is a loop.
constexpr int k_max_pointer_hops = 16;

std::uint16_t read_u16(const std::vector<std::uint8_t>& d, std::size_t at)
{
  return static_cast<std::uint16_t>((d[at] << 8) | d[at + 1]);
}

void push_u16(std::vector<std::uint8_t>& out, std::uint16_t value)
{
  out.push_back(static_cast<std::uint8_t>(value >> 8));
  out.push_back(static_cast<std::uint8_t>(value & 0xFF));
}

// Split a dotted name into labels. mDNS is case-insensitive, so comparisons
// fold case, but the original bytes are preserved for reporting.
std::vector<std::string> labels(const std::string& name)
{
  std::vector<std::string> out;
  std::size_t start = 0;
  while (start < name.size()) {
    const auto dot = name.find('.', start);
    const auto end = dot == std::string::npos ? name.size() : dot;
    if (end > start) {
      out.push_back(name.substr(start, end - start));
    }
    start = end + 1;
  }
  return out;
}

std::string lowercase(std::string text)
{
  std::transform(text.begin(),
                 text.end(),
                 text.begin(),
                 [](unsigned char c)
                 { return static_cast<char>(std::tolower(c)); });
  return text;
}

// Read a DNS name at `start`, following compression pointers. `next` receives
// the offset just past the name as it appears at `start`, so a caller can
// continue.
std::string read_name(const std::vector<std::uint8_t>& d,
                      std::size_t start,
                      std::size_t* next,
                      bool* bad)
{
  std::string out;
  std::size_t pos = start;
  std::size_t resume = start;
  bool jumped = false;
  int hops = 0;

  for (;;) {
    if (pos >= d.size()) {
      *bad = true;
      return {};
    }
    const std::uint8_t length = d[pos];

    if (length == 0) {
      ++pos;
      if (!jumped) {
        resume = pos;
      }
      break;
    }

    if ((length & 0xC0) == 0xC0) {
      if (pos + 1 >= d.size()) {
        *bad = true;
        return {};
      }
      const std::size_t target =
          (static_cast<std::size_t>(length & 0x3F) << 8) | d[pos + 1];
      if (!jumped) {
        resume = pos + 2;
      }
      jumped = true;
      if (++hops > k_max_pointer_hops || target >= d.size()) {
        *bad = true;
        return {};
      }
      pos = target;
      continue;
    }

    // 0x40/0x80 are reserved and would let a packet claim an absurd label.
    if ((length & 0xC0) != 0 || pos + 1 + length > d.size()) {
      *bad = true;
      return {};
    }
    if (!out.empty()) {
      out.push_back('.');
    }
    out.append(reinterpret_cast<const char*>(&d[pos + 1]), length);
    pos += 1 + length;
  }

  *next = resume;
  return out;
}

std::string dotted_ipv4(const std::vector<std::uint8_t>& d, std::size_t at)
{
  return std::to_string(d[at]) + "." + std::to_string(d[at + 1]) + "."
      + std::to_string(d[at + 2]) + "." + std::to_string(d[at + 3]);
}

std::string ipv6_text(const std::vector<std::uint8_t>& d, std::size_t at)
{
  std::string out;
  for (int group = 0; group < 8; ++group) {
    const auto value = static_cast<std::uint16_t>((d[at + group * 2] << 8)
                                                  | d[at + group * 2 + 1]);
    if (group != 0) {
      out.push_back(':');
    }
    char buffer[5];
    std::snprintf(buffer, sizeof(buffer), "%x", value);
    out += buffer;
  }
  return out;
}

// The instance label of a fully qualified name, e.g.
// "rist2rist-aa._obr-rist._udp.local" -> "rist2rist-aa".
std::string instance_of(const std::string& fqdn)
{
  const auto dot = fqdn.find('.');
  return dot == std::string::npos ? fqdn : fqdn.substr(0, dot);
}

bool belongs_to_service(const std::string& fqdn, const std::string& wanted)
{
  const std::string name = lowercase(fqdn);
  if (name.size() <= wanted.size()) {
    return false;
  }
  const auto at = name.size() - wanted.size();
  return name.compare(at, wanted.size(), wanted) == 0 && name[at - 1] == '.';
}
}  // namespace

std::string service::txt_value(const std::string& key) const
{
  const auto it = txt.find(key);
  return it == txt.end() ? std::string {} : it->second;
}

std::vector<std::uint8_t> encode_query(const std::string& service_name)
{
  std::vector<std::uint8_t> out;
  out.reserve(64);

  push_u16(out, 0);  // id: mDNS answers ignore it
  push_u16(out, 0);  // flags: a standard query
  push_u16(out, 1);  // qdcount
  push_u16(out, 0);  // ancount
  push_u16(out, 0);  // nscount
  push_u16(out, 0);  // arcount

  for (const auto& label : labels(service_name)) {
    if (label.size() > 63) {
      continue;  // cannot be encoded; a valid service name never does this
    }
    out.push_back(static_cast<std::uint8_t>(label.size()));
    out.insert(out.end(), label.begin(), label.end());
  }
  out.push_back(0);  // root

  push_u16(out, k_type_ptr);
  push_u16(out, 1);  // class IN
  return out;
}

std::vector<service> parse_response(const std::vector<std::uint8_t>& packet,
                                    const std::string& service_name)
{
  std::vector<service> found;
  if (packet.size() < k_header_size) {
    return found;
  }

  const auto question_count = read_u16(packet, 4);
  const auto answer_count = read_u16(packet, 6);
  const auto authority_count = read_u16(packet, 8);
  const auto additional_count = read_u16(packet, 10);

  const std::string wanted = lowercase(service_name);

  std::map<std::string, service> by_name;  // keyed by the full instance name
  std::map<std::string, std::string> addresses;  // host name -> address text

  std::size_t pos = k_header_size;
  bool bad = false;

  // Skip the questions: a response repeats the query, and we already know it.
  for (std::uint16_t i = 0; i < question_count; ++i) {
    read_name(packet, pos, &pos, &bad);
    if (bad || pos + 4 > packet.size()) {
      return {};
    }
    pos += 4;
  }

  const std::uint32_t records = static_cast<std::uint32_t>(answer_count)
      + authority_count + additional_count;
  for (std::uint32_t i = 0; i < records; ++i) {
    const std::string name = read_name(packet, pos, &pos, &bad);
    if (bad || pos + 10 > packet.size()) {
      return {};
    }

    const auto type = read_u16(packet, pos);
    const auto rdata_length = read_u16(packet, pos + 8);
    pos += 10;

    if (pos + rdata_length > packet.size()) {
      return {};
    }
    const std::size_t rdata = pos;
    pos = rdata + rdata_length;  // advance past the rdata unconditionally

    if (type == k_type_ptr) {
      std::size_t next = rdata;
      const std::string target = read_name(packet, rdata, &next, &bad);
      if (bad) {
        return {};
      }
      if (belongs_to_service(target, wanted)) {
        auto& entry = by_name[target];
        entry.instance = instance_of(target);
      }
    } else if (type == k_type_srv && rdata_length >= 6) {
      // The PTR answers are the authority on which instances exist for our
      // service; a SRV/TXT for anything else on the LAN is not ours to report.
      if (!belongs_to_service(name, wanted)) {
        continue;
      }
      auto& entry = by_name[name];
      entry.instance = instance_of(name);
      entry.port = read_u16(packet, rdata + 4);
      std::size_t next = rdata + 6;
      entry.host = read_name(packet, rdata + 6, &next, &bad);
      if (bad) {
        return {};
      }
    } else if (type == k_type_txt) {
      if (!belongs_to_service(name, wanted)) {
        continue;
      }
      auto& entry = by_name[name];
      entry.instance = instance_of(name);
      // TXT rdata is a sequence of length-prefixed strings, conventionally
      // "key=value".
      std::size_t at = rdata;
      const std::size_t end = rdata + rdata_length;
      while (at < end) {
        const std::size_t length = packet[at];
        if (at + 1 + length > end) {
          return {};
        }
        const std::string text(reinterpret_cast<const char*>(&packet[at + 1]),
                               length);
        const auto equals = text.find('=');
        if (equals != std::string::npos) {
          entry.txt[text.substr(0, equals)] = text.substr(equals + 1);
        } else if (!text.empty()) {
          entry.txt[text] = "";
        }
        at += 1 + length;
      }
    } else if (type == k_type_a && rdata_length == 4) {
      addresses[lowercase(name)] = dotted_ipv4(packet, rdata);
    } else if (type == k_type_aaaa && rdata_length == 16) {
      addresses[lowercase(name)] = ipv6_text(packet, rdata);
    }
  }

  for (auto& [name, entry] : by_name) {
    const auto it = addresses.find(lowercase(entry.host));
    if (it != addresses.end()) {
      entry.address = it->second;
    }
    found.push_back(std::move(entry));
  }
  return found;
}

}  // namespace bridge::mdns
