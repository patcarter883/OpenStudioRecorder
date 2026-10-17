// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Pat Carter
//
// Finding bridges on the LAN (DT-19).
//
// Discovery is a convenience, never a dependency: a manual address is always
// available, and a browse that finds nothing is an ordinary outcome, not an
// error.

#pragma once

#include <chrono>
#include <vector>

#include "bridge/mdns.h"

namespace bridge
{

// Fold a newly parsed advertisement into what has already been seen. Responders
// split PTR, SRV, TXT and A across separate records and often separate packets,
// so the instance is the join key. Pure, so the merge is testable on its own.
void merge_service(std::vector<mdns::service>& seen,
                   const mdns::service& incoming);

// Browse the LAN for services of `service_name` (default: the bridge type).
// BLOCKING: call it from a background thread, never from the UI. Returns
// everything that answered within the window.
std::vector<mdns::service> discover(
    std::chrono::milliseconds window,
    const std::string& service_name = mdns::k_service);

}  // namespace bridge
