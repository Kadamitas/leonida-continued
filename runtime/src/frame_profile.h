#pragma once

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace leonida {

// An opt-in wall-clock breakdown. Each owner serializes its own queue or presenter.
class FrameProfile {
    using Clock = std::chrono::steady_clock;
public:
    using Time = Clock::time_point;
    static Time now() {
        static const bool enabled = [] {
            const char* value = std::getenv("LEONIDA_PROFILE_FRAMES");
            return value && std::strcmp(value, "1") == 0;
        }();
        return enabled ? Clock::now() : Time{};
    }

    void record(const char* stages, Time start, Time a, Time b, Time c) {
        if (start == Time{}) return;
        const Time end = Clock::now();
        const std::array<Time, 5> marks{start, a, b, c, end};
        for (unsigned i = 0; i < 4; ++i) totals_[i] += marks[i + 1] - marks[i];
        ++count_;
        if (since_ == Time{}) since_ = start;
        if (end - since_ < std::chrono::seconds(4)) return;
        std::array<double, 4> ms{};
        for (unsigned i = 0; i < 4; ++i)
            ms[i] = std::chrono::duration<double, std::milli>(totals_[i]).count() / count_;
        std::fprintf(stderr, "[rt] frame profile %s: count=%u seconds=%.3f mean_ms=%.3f,%.3f,%.3f,%.3f\n",
                     stages, count_, std::chrono::duration<double>(end - since_).count(), ms[0], ms[1], ms[2], ms[3]);
        totals_ = {};
        count_ = 0;
        since_ = end;
    }
private:
    Time since_{};
    std::array<Clock::duration, 4> totals_{};
    unsigned count_ = 0;
};

} // namespace leonida
