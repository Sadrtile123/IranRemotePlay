// Phase 15 tests — adaptive bitrate hysteresis, smoothing, bounds.

#include <vector>
#include "adapt/AdaptiveBitrate.h"

#include "TestHarness.hpp"

using namespace rp::media;

RP_TEST(abr_holds_steady_on_healthy_link) {
    AdaptiveBitrate abr(8000);
    LinkSample good;   // defaults = healthy
    for (int i = 0; i < 20; ++i) {
        const auto next = abr.update(good);
        if (next) CHECK(*next > 8000);   // may climb slowly, never drops
    }
    CHECK(abr.currentKbps() >= 8000);
}

RP_TEST(abr_steps_down_smoothly_after_streak) {
    AdaptiveBitrate abr(8000);
    LinkSample bad;    // defaults = degraded thresholds
    bad.lossPercent = 5.0;
    int changes = 0;
    int last = 8000;
    for (int i = 0; i < 60; ++i) {
        const auto next = abr.update(bad);
        if (next) {
            ++changes;
            // steps are proportional and bounded (8 -> ~6.8 -> ~5.8 Mbps)
            CHECK(*next < last);
            CHECK(last - *next <= 1200 + 250);
            last = *next;
        }
    }
    CHECK(changes >= 3);              // kept stepping down
    CHECK(abr.currentKbps() >= 2000); // floor respected
}

RP_TEST(abr_no_oscillation) {
    AdaptiveBitrate abr(8000);
    // Alternating bad/good samples must never change the rate: streaks never
    // complete, hysteresis holds.
    for (int i = 0; i < 40; ++i) {
        LinkSample bad; bad.lossPercent = 4.0;
        LinkSample good;
        CHECK(!abr.update(bad).has_value());
        CHECK(!abr.update(good).has_value());
    }
    CHECK_EQ(abr.currentKbps(), 8000);
}

RP_TEST(abr_recovers_upward_slowly) {
    AdaptiveBitrate abr(4000);
    LinkSample good;
    int ups = 0;
    for (int i = 0; i < 100; ++i) {
        if (abr.update(good)) ++ups;
    }
    CHECK(ups >= 2);                          // climbs after upStreakNeeded
    CHECK(abr.currentKbps() > 4000);          // higher than start
    CHECK(abr.currentKbps() <= 30000);        // ceiling respected
}

RP_TEST(abr_cooldown_enforced) {
    AdaptiveBitrate abr(10000);
    LinkSample bad; bad.lossPercent = 9.0;
    // Track sample indices where the rate changed: consecutive changes must
    // be at least (cooldown + downStreakNeeded) samples apart.
    std::vector<int> changeAt;
    for (int i = 0; i < 40; ++i) {
        if (abr.update(bad)) changeAt.push_back(i);
    }
    CHECK(changeAt.size() >= 3);       // it keeps stepping down over 40 samples
    CHECK(changeAt.size() <= 14);      // spacing-bounded, not every sample
    for (size_t i = 1; i < changeAt.size(); ++i) {
        CHECK(changeAt[i] - changeAt[i - 1] >= 3);   // streak alone needs 3 samples
    }
}

RP_TEST(abr_neutral_zone_resets_streaks) {
    AdaptiveBitrate abr(8000);
    LinkSample meh;   // neither bad nor good
    meh.lossPercent = 1.0;
    meh.rttMs = 100.0;
    for (int i = 0; i < 30; ++i) CHECK(!abr.update(meh).has_value());
    CHECK_EQ(abr.currentKbps(), 8000);
}

RP_TEST(abr_manual_override) {
    AdaptiveBitrate abr(8000);
    abr.setManual(5000);
    CHECK_EQ(abr.currentKbps(), 5000);
    LinkSample good;
    for (int i = 0; i < 12; ++i) abr.update(good);
    CHECK(abr.currentKbps() > 5000);   // resumes adaptation upward
}

RP_TEST_MAIN("abr")
