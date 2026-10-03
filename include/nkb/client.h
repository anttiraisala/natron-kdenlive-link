// client.h - connecting and authenticating to the daemon. Used by the mock
// tools, the cache/doctor tools and (milestone 2) the MLT filter.
#pragma once

#include <string>

#include "nkb/net.h"
#include "nkb/protocol.h"

namespace nkb {

// Connects, sends Hello(role, token) and waits for HelloAck.
// Returns an invalid Socket and fills *err on failure.
Socket connect_and_handshake(const Address& addr, Role role, const std::string& token,
                             int timeout_ms, std::string* err);

// One-shot control command ("ping", "stats", "clear") on the filter address.
bool control_request(const Address& addr, const std::string& token, const std::string& cmd,
                     std::string* reply, std::string* err);

}  // namespace nkb
