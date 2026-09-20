// RemotePlay - common/Version.h
// Application-wide version and identity constants.
#pragma once

#include <cstdint>

namespace rp {

inline constexpr const char* kAppName = "RemotePlay";
inline constexpr const char* kAppVersion = "0.1.1"; // stability + UI release

// Default TCP listen port for the host (direct-connection phase).
// The signaling server (Phase 13) will remove the need for users to know this.
inline constexpr uint16_t kDefaultListenPort = 41717;

// Maximum number of simultaneous clients (players) in a session.
inline constexpr size_t kMaxClients = 4;

} // namespace rp
