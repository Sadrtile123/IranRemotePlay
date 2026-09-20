// RemotePlay - common/Log.h
// Thread-safe structured logging with console + rotating file sinks.
// Output format: [LEVEL] [YYYY-MM-DD HH:MM:SS.mmm] [tid xxxxx] message
// Files live in %LOCALAPPDATA%\RemotePlay\Logs on Windows (see Paths.h).
#pragma once

#include <sstream>
#include <string>

namespace rp::log {

enum class Level {
    Trace = 0,
    Debug = 1,
    Info = 2,
    Warning = 3,
    Error = 4,
    Critical = 5,
};

[[nodiscard]] const char* levelName(Level l);

// Initialize the file sink (append). Creates the directory if needed.
// Safe to call more than once: switches to the new directory.
// Without any init() call the logger writes to console only.
void init(const std::string& logDirectory);

// Adjust minimum levels (applies to subsequent lines).
void setConsoleLevel(Level min);
void setFileLevel(Level min);

// Write one line. Thread-safe.
void write(Level level, const std::string& message);

namespace detail {

// Stream proxy: collects << items and flushes in the destructor.
// If the level is filtered out the proxy discards everything cheaply.
class Line {
public:
    explicit Line(Level level);
    Line(Line&& other) noexcept;
    ~Line();

    template <typename T>
    Line& operator<<(const T& value) {
        if (active_) stream_ << value;
        return *this;
    }

private:
    Level level_;
    bool active_;
    std::ostringstream stream_;
};

} // namespace detail

} // namespace rp::log

#define RP_LOG(lvl) ::rp::log::detail::Line(::rp::log::Level::lvl)

#define RP_TRACE() RP_LOG(Trace)
#define RP_DEBUG() RP_LOG(Debug)
#define RP_INFO() RP_LOG(Info)
#define RP_WARN() RP_LOG(Warning)
#define RP_ERROR() RP_LOG(Error)
#define RP_CRIT() RP_LOG(Critical)
