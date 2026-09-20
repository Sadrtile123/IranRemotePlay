// RemotePlay - common/Paths.h
// Platform directory helpers.
// Windows: %APPDATA%\RemotePlay (config), %LOCALAPPDATA%\RemotePlay\Logs (logs)
// Linux (dev/CI): ~/.config/RemotePlay, ~/.local/state/RemotePlay/Logs
#pragma once

#include <string>
#include <vector>

namespace rp::paths {

// Directory that stores the JSON configuration (created on demand).
[[nodiscard]] std::string configDir();
[[nodiscard]] std::string configFilePath(); // configDir() + "\config.json"

// Directory that stores log files (created on demand).
[[nodiscard]] std::string logDir();

// Creates all missing parent directories; returns the input path.
[[nodiscard]] std::string ensureDirectoryExists(const std::string& dir);

// Best-effort display name of the current user ("Mahdyar"). Falls back to
// "Host"/"Player" style placeholders at call sites.
[[nodiscard]] std::string userName();

// Local non-loopback IPv4 addresses for showing the host's connectable IPs.
[[nodiscard]] std::vector<std::string> localIPv4Addresses();

} // namespace rp::paths
