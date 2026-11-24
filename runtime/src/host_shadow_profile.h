#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace leonida {

// Opt-in CPU time and staging footprint for each texture class. Queue execution is serialized.
class HostShadowProfile {
    using Clock = std::chrono::steady_clock;
    using Duration = Clock::duration;

public:
    enum Class : unsigned { large_r8, bc, other_mutable, immutable, classes };
    struct Sample {
        Duration time{};
        std::uint64_t calls{}, dirty{}, staging_bytes{};
    };
    using Samples = std::array<Sample, classes>;
    using Time = Clock::time_point;

    bool enabled() const { return enabled_; }

    static void capture(Samples& samples, Class kind, Time start, std::uint64_t staging_bytes, bool dirty) {
        auto& sample = samples[kind];
        sample.time += Clock::now() - start;
        ++sample.calls;
        sample.staging_bytes += staging_bytes;
        sample.dirty += dirty;
    }

    void record(const Samples& samples, std::uint64_t reasons, const std::array<const char*, 3>& names, std::size_t selected,
                Time begin, Time wait_begin, Time stage_begin, Time end) {
        ++batches_;
        full_scans_ += reasons != 0;
        reasons_ |= reasons;
        if (reasons) for (unsigned i = 0; i < names.size(); ++i) {
            if (!names[i]) continue;
            auto& source = last_names_[i];
            if (std::strncmp(source.data(), names[i], source.size() - 1) != 0)
                std::snprintf(source.data(), source.size(), "%s", names[i]);
        }
        selected_ += selected;
        waiting_ += stage_begin - wait_begin;
        staging_ += end - stage_begin;
        validation_ += wait_begin - begin;
        for (unsigned i = 0; i < classes; ++i) {
            totals_[i].calls += samples[i].calls;
            totals_[i].dirty += samples[i].dirty;
            totals_[i].staging_bytes += samples[i].staging_bytes;
            totals_[i].time += samples[i].time;
            validation_ -= samples[i].time;
        }
        if (since_ == Time{}) since_ = begin;
        if (end - since_ < std::chrono::seconds(4)) return;
        const auto ms = [&](Duration elapsed) {
            return std::chrono::duration<double, std::milli>(elapsed).count() / batches_;
        };
        std::fprintf(stderr, "[rt] host shadows batches=%u full=%u reason_mask=0x%llx selected=%.2f "
                     "mean_ms(validate,wait,stage)=%.3f,%.3f,%.3f", batches_, full_scans_, static_cast<unsigned long long>(reasons_),
                     static_cast<double>(selected_) / batches_, ms(validation_), ms(waiting_), ms(staging_));
        constexpr std::array labels{"r8-1080p+", "bc", "other", "static"};
        for (unsigned i = 0; i < classes; ++i) {
            const auto& total = totals_[i];
            std::fprintf(stderr, " %s(calls,dirty,stagingMiB,ms)=%.2f,%.2f,%.2f,%.3f", labels[i],
                         static_cast<double>(total.calls) / batches_, static_cast<double>(total.dirty) / batches_,
                         static_cast<double>(total.staging_bytes) / (batches_ * 1048576.0), ms(total.time));
        }
        constexpr std::array stages{"cs", "vs", "ps"};
        for (unsigned i = 0; i < stages.size(); ++i) {
            if (last_names_[i][0]) std::fprintf(stderr, " last-unbounded-%s=%s", stages[i], last_names_[i].data());
            last_names_[i][0] = 0;
        }
        std::fputc('\n', stderr);
        batches_ = full_scans_ = 0;
        selected_ = 0;
        reasons_ = 0;
        totals_ = {};
        waiting_ = staging_ = validation_ = {};
        since_ = end;
    }

    static Time now() { return Clock::now(); }

private:
    bool enabled_ = [] {
        const char* value = std::getenv("LEONIDA_PROFILE_SHADOWS");
        return value && std::strcmp(value, "1") == 0;
    }();
    unsigned batches_{}, full_scans_{};
    std::uint64_t selected_{};
    std::uint64_t reasons_{};
    std::array<std::array<char, 192>, 3> last_names_{};
    Samples totals_{};
    Duration waiting_{}, staging_{}, validation_{};
    Time since_{};
};

} // namespace leonida
