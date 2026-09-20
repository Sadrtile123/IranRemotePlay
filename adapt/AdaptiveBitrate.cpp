// Phase 15 — adaptive bitrate implementation. See AdaptiveBitrate.h.

#include <optional>
#include "AdaptiveBitrate.h"

#include <algorithm>

namespace rp::media {

AdaptiveBitrate::AdaptiveBitrate(int initialKbps, int minKbps, int maxKbps)
    : currentKbps_(std::clamp(initialKbps, minKbps, maxKbps)),
      minKbps_(minKbps), maxKbps_(maxKbps) {}

bool AdaptiveBitrate::isBad(const LinkSample& s) const {
    if (s.lossPercent >= badLossPercent) return true;
    if (s.rttMs >= badRttMs) return true;
    if (s.jitterMs >= badJitterMs) return true;
    if (s.decodeQueueFrames >= badQueueFrames) return true;
    if (s.fpsRatio > 0.0 && s.fpsRatio <= badFpsRatio) return true;
    return false;
}

bool AdaptiveBitrate::isGood(const LinkSample& s) const {
    if (s.lossPercent > goodLossPercent) return false;
    if (s.rttMs > badRttMs) return false;         // not even mediocre
    if (s.jitterMs > goodJitterMs) return false;
    if (s.decodeQueueFrames >= 2.0) return false;
    if (s.fpsRatio > 0.0 && s.fpsRatio < goodFpsRatio) return false;
    return true;
}

std::optional<int> AdaptiveBitrate::update(const LinkSample& s) {
    if (cooldown_ > 0) --cooldown_;

    if (isBad(s)) {
        ++downStreak_;
        upStreak_ = 0;
        if (downStreak_ >= downStreakNeeded && cooldown_ == 0) {
            const int step = std::max(250, static_cast<int>(currentKbps_ * downStepFraction));
            const int next = std::max(minKbps_, currentKbps_ - step);
            downStreak_ = 0;
            if (next != currentKbps_) {
                currentKbps_ = next;
                cooldown_ = cooldownSamples;
                return next;
            }
        }
        return std::nullopt;
    }

    if (isGood(s)) {
        ++upStreak_;
        downStreak_ = 0;
        if (upStreak_ >= upStreakNeeded && cooldown_ == 0 && currentKbps_ < maxKbps_) {
            const int step = std::max(250, static_cast<int>(currentKbps_ * upStepFraction));
            const int next = std::min(maxKbps_, currentKbps_ + step);
            upStreak_ = 0;
            if (next != currentKbps_) {
                currentKbps_ = next;
                cooldown_ = cooldownSamples;
                return next;
            }
        }
        return std::nullopt;
    }

    // Neutral zone: reset both streaks (hysteresis).
    downStreak_ = 0;
    upStreak_ = 0;
    return std::nullopt;
}

void AdaptiveBitrate::setManual(int kbps) {
    currentKbps_ = std::clamp(kbps, minKbps_, maxKbps_);
    downStreak_ = upStreak_ = 0;
    cooldown_ = cooldownSamples;
}

} // namespace rp::media
