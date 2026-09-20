// RemotePlay - common/Log.cpp
#include "common/Log.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <thread>

namespace rp::log {

namespace {

namespace fs = std::filesystem;

struct Sink {
    std::mutex mutex;
    std::ofstream file;
    std::string filePath;
    Level consoleMin = Level::Info;
    Level fileMin = Level::Debug;
    bool initialized = false; // file sink active
};

Sink& sink() {
    static Sink s;
    return s;
}

std::string timestamp() {
    using namespace std::chrono;
    const auto now = system_clock::now();
    const auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
    const std::time_t tt = system_clock::to_time_t(now);
    std::tm tmv{};
#ifdef _WIN32
    localtime_s(&tmv, &tt);
#else
    localtime_r(&tt, &tmv);
#endif
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d.%03d",
                  tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                  tmv.tm_hour, tmv.tm_min, tmv.tm_sec, static_cast<int>(ms.count()));
    return buf;
}

std::string threadTag() {
    const auto h = std::hash<std::thread::id>{}(std::this_thread::get_id());
    char buf[24];
    std::snprintf(buf, sizeof(buf), "tid %llx", static_cast<unsigned long long>(h));
    return buf;
}

void pruneOldLogs(const fs::path& dir, int keepDays) {
    std::error_code ec;
    if (!fs::exists(dir, ec)) return;
    const auto cutoff = fs::file_time_type::clock::now() - std::chrono::hours(24 * keepDays);
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file()) continue;
        const std::string name = entry.path().filename().string();
        if (name.rfind("remoteplay_", 0) != 0) continue;
        std::error_code tec;
        const auto t = entry.last_write_time(tec);
        if (tec) continue;
        if (t < cutoff) {
            fs::remove(entry.path(), ec);
        }
    }
}

} // namespace

const char* levelName(Level l) {
    switch (l) {
        case Level::Trace: return "TRACE";
        case Level::Debug: return "DEBUG";
        case Level::Info: return "INFO";
        case Level::Warning: return "WARNING";
        case Level::Error: return "ERROR";
        case Level::Critical: return "CRITICAL";
    }
    return "OFF";
}

void init(const std::string& logDirectory) {
    std::error_code ec;
    fs::path dir(logDirectory);
    fs::create_directories(dir, ec);
    if (ec) {
        RP_WARN() << "Log directory '" << logDirectory << "' could not be created: " << ec.message();
        return;
    }
    pruneOldLogs(dir, 7);

    fs::path path = dir / (std::string("remoteplay_") + timestamp().substr(0, 10) + ".log");
    std::ofstream f(path, std::ios::app);
    if (!f.is_open()) {
        RP_WARN() << "Log file '" << path.string() << "' could not be opened; console only.";
        return;
    }
    Sink& s = sink();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.file = std::move(f);
    s.filePath = path.string();
    s.initialized = true;
    s.file << "[" << levelName(Level::Info) << "] [" << timestamp() << "] [" << threadTag()
           << "] Log file opened: " << s.filePath << "\n";
}

void setConsoleLevel(Level min) {
    std::lock_guard<std::mutex> lock(sink().mutex);
    sink().consoleMin = min;
}

void setFileLevel(Level min) {
    std::lock_guard<std::mutex> lock(sink().mutex);
    sink().fileMin = min;
}

void write(Level level, const std::string& message) {
    Sink& s = sink();
    const bool toConsole = level >= s.consoleMin;
    const bool toFile = s.initialized && level >= s.fileMin;
    if (!toConsole && !toFile) return;

    const std::string line = std::string("[") + levelName(level) + "] [" + timestamp() + "] [" +
                              threadTag() + "] " + message;
    if (toConsole) {
        std::fprintf(level >= Level::Error ? stderr : stdout, "%s\n", line.c_str());
        std::fflush(level >= Level::Error ? stderr : stdout);
    }
    if (toFile) {
        std::lock_guard<std::mutex> lock(s.mutex);
        s.file << line << '\n';
        if (level >= Level::Error) s.file.flush();
    }
}

namespace detail {

Line::Line(Level level) : level_(level), active_(level >= sink().consoleMin ||
                                                 (sink().initialized && level >= sink().fileMin)) {}

Line::Line(Line&& other) noexcept : level_(other.level_), active_(other.active_) {
    other.active_ = false;
}

Line::~Line() {
    if (!active_) return;
    write(level_, stream_.str());
}

} // namespace detail

} // namespace rp::log
