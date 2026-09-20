// RemotePlay - common/Paths.cpp
#include <stdexcept>
#include <string>
#include <vector>
#include "common/Paths.h"

#include "common/Log.h"

#include <asio.hpp>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#include <objbase.h>
#else
#include <unistd.h>
#endif

namespace rp::paths {

namespace fs = std::filesystem;

namespace {

#ifdef _WIN32

// UTF-16 -> UTF-8 (Windows paths can contain non-ASCII characters).
std::string wideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                                         nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string out(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), size,
                        nullptr, nullptr);
    return out;
}

std::string knownFolderPath(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    const HRESULT hr = SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &p);
    if (FAILED(hr) || p == nullptr) return {};
    std::wstring w(p);
    CoTaskMemFree(p);
    return wideToUtf8(w);
}

std::string appDataDir() {
    std::string base = knownFolderPath(FOLDERID_RoamingAppData);
    if (base.empty()) {
        const char* env = std::getenv("APPDATA");
        if (env) base = env;
    }
    if (base.empty()) return ".";
    return base + "\\RemotePlay";
}

std::string localAppDataDir() {
    std::string base = knownFolderPath(FOLDERID_LocalAppData);
    if (base.empty()) {
        const char* env = std::getenv("LOCALAPPDATA");
        if (env) base = env;
    }
    if (base.empty()) return ".";
    return base + "\\RemotePlay";
}

#else // Linux / dev-CI build

std::string homeDir() {
    const char* env = std::getenv("HOME");
    if (env && *env) return env;
    return ".";
}

std::string appDataDir() { return homeDir() + "/.config/RemotePlay"; }
std::string localAppDataDir() { return homeDir() + "/.local/state/RemotePlay"; }

#endif

} // namespace

std::string ensureDirectoryExists(const std::string& dir) {
    if (dir.empty()) return dir;
    std::error_code ec;
    fs::create_directories(fs::path(dir), ec);
    if (ec) {
        RP_WARN() << "Could not create directory '" << dir << "': " << ec.message();
    }
    return dir;
}

std::string configDir() { return appDataDir(); }

std::string configFilePath() {
    const fs::path p = fs::path(appDataDir()) / "config.json";
    return p.string();
}

std::string logDir() {
    const fs::path base = localAppDataDir();
    return ensureDirectoryExists((base / "Logs").string());
}

std::string userName() {
#ifdef _WIN32
    const char* env = std::getenv("USERNAME");
#else
    const char* env = std::getenv("USER");
#endif
    if (env && *env) return env;
    return {};
}

std::vector<std::string> localIPv4Addresses() {
    std::vector<std::string> result;
    try {
        std::array<char, 256> host{};
        if (::gethostname(host.data(), static_cast<int>(host.size()) - 1) != 0) return result;
        asio::io_context io;
        asio::ip::tcp::resolver resolver(io);
        asio::error_code ec;
        const auto results = resolver.resolve(host.data(), "", ec);
        if (ec) return result;
        for (const auto& entry : results) {
            const auto addr = entry.endpoint().address();
            if (!addr.is_v4()) continue;
            const auto bytes = addr.to_v4().to_bytes();
            const bool loopback = bytes[0] == 127;
            const bool linkLocal = bytes[0] == 169 && bytes[1] == 254;
            if (loopback || linkLocal) continue;
            const std::string s = addr.to_string();
            if (std::find(result.begin(), result.end(), s) == result.end()) result.push_back(s);
        }
    } catch (const std::exception& e) {
        RP_DEBUG() << "Address enumeration failed: " << e.what();
    }
    return result;
}

} // namespace rp::paths
