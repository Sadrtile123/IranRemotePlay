#pragma once
// Phase 15 — adaptive bitrate controller.
//
// Inputs (sampled once per second by the host from live UDP stats):
//   rttMs, lossPercent, jitterMs, decodeQueueBacklog, fpsRatio (achieved/target)
//
// Behavior: smooth, conservative, hysteresis-first. The controller never
// oscillates: it steps DOWN only after `kDownStreak` consecutive degraded
// samples, steps UP only after `kUpStreak` consecutive healthy samples, and
// enforces a cooldown between any two changes. Step size is proportional
// (10-15%), like the spec's example of 8 -> 7 -> 6 Mbps gradual decrease.
//
// Pure logic; unit-tested on all platforms.

#include <cstdint>
#include <optional>

namespace rp::media {

struct LinkSample {
    double rttMs = 0.0;             // EWMA RTT
    double lossPercent = 0.0;       // lost frames / expected frames * 100
    double jitterMs = 0.0;          // inter-arrival jitter
    double decodeQueueFrames = 0.0; // client decode backlog (0..N)
    double fpsRatio = 1.0;          // achieved / target fps
};

class AdaptiveBitrate {
public:
    AdaptiveBitrate(int initialKbps, int minKbps = 2000, int maxKbps = 30000);

    // One sample per second. Returns the NEW target when it changes,
    // nullopt otherwise.
    [[nodiscard]] std::optional<int> update(const LinkSample& s);

    [[nodiscard]] int currentKbps() const { return currentKbps_; }
    [[nodiscard]] bool degraded() const { return downStreak_ > 0; }

    // Force a target (host UI slider). Resets streaks + cooldown.
    void setManual(int kbps);

    // Thresholds (tunable, exposed for tests).
    double badRttMs = 150.0;
    double badLossPercent = 2.0;
    double badJitterMs = 15.0;
    double badQueueFrames = 4.0;
    double badFpsRatio = 0.80;
    double goodRttMs = 80.0;
    double goodLossPercent = 0.3;
    double goodJitterMs = 5.0;
    double goodFpsRatio = 0.95;
    int downStreakNeeded = 3;       // seconds of bad stats before stepping down
    int upStreakNeeded = 10;        // seconds of good stats before stepping up
    int cooldownSamples = 2;        // min seconds between changes
    double downStepFraction = 0.15; // -15% per step
    double upStepFraction = 0.10;   // +10% per step

private:
    [[nodiscard]] bool isBad(const LinkSample& s) const;
    [[nodiscard]] bool isGood(const LinkSample& s) const;

    int currentKbps_;
    int minKbps_;
    int maxKbps_;
    int downStreak_ = 0;
    int upStreak_ = 0;
    int cooldown_ = 0;
};

} // namespace rp::media
