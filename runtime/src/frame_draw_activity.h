#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace leonida {

// Bounded diagnostic data. Dropping additional attachments can undercount a busy frame.
struct FrameDrawTargets {
    struct Entry {
        std::uintptr_t resource;
        std::uintptr_t queue;
        std::uint32_t draws;
        std::uint32_t meshes;
    };
    std::array<Entry, 64> entries;
    unsigned count = 0;
    bool truncated = false;

    void add(Entry value) {
        for (unsigned i = 0; i < count; ++i) {
            auto& old = entries[i];
            if (old.resource == value.resource && old.queue == value.queue) {
                old.draws += value.draws;
                old.meshes += value.meshes;
                return;
            }
        }
        if (count < entries.size()) entries[count++] = value;
        else truncated = true;
    }
};

// All calls run under the VulkanContext submission lock. A match means at least one nonempty,
// unpredicated direct draw named the primary present source on this queue since the last accepted present.
class FrameDrawActivity {
public:
    static bool enabled() {
        static const bool active = [] {
            const char* s = std::getenv("LEONIDA_FRAME_DRAWS");
            return s && s[0] == '1' && s[1] == '\0';
        }();
        return active;
    }
    void submit(std::uintptr_t queue, const FrameDrawTargets& batch) {
        pending_.truncated |= batch.truncated;
        for (unsigned i = 0; i < batch.count; ++i) {
            auto entry = batch.entries[i];
            entry.queue = queue;
            pending_.add(entry);
        }
    }
    struct Report {
        std::uint64_t presents = 0, matched = 0, draws = 0, meshes = 0, truncated = 0;
        double seconds = 0;
        std::array<std::uintptr_t, 4> primary_ids{};
        void print() const {
            if (seconds > 0)
                std::fprintf(stderr, "[rt] present draw activity: seconds=%.3f presents=%llu primary_draw_presents=%llu "
                             "primary_draws=%llu primary_mesh_draws=%llu truncated_intervals=%llu "
                             "sampled_primary_ids=0x%llx,0x%llx,0x%llx,0x%llx\n",
                             seconds, static_cast<unsigned long long>(presents), static_cast<unsigned long long>(matched),
                             static_cast<unsigned long long>(draws), static_cast<unsigned long long>(meshes),
                             static_cast<unsigned long long>(truncated), static_cast<unsigned long long>(primary_ids[0]),
                             static_cast<unsigned long long>(primary_ids[1]), static_cast<unsigned long long>(primary_ids[2]),
                             static_cast<unsigned long long>(primary_ids[3]));
        }
    };
    using Clock = std::chrono::steady_clock;
    Report present(std::uintptr_t queue, std::uintptr_t primary, Clock::time_point now) {
        if (started_) {
            ++window_.presents;
            for (auto& id : window_.primary_ids) {
                if (id == primary) break;
                if (!id) { id = primary; break; }
            }
            window_.truncated += pending_.truncated;
            for (unsigned i = 0; i < pending_.count; ++i) {
                const auto& e = pending_.entries[i];
                if (e.resource == primary && e.queue == queue) {
                    ++window_.matched;
                    window_.draws += e.draws;
                    window_.meshes += e.meshes;
                    break;
                }
            }
        } else {
            started_ = true;
            start_ = now;
        }
        pending_.count = 0;
        pending_.truncated = false;
        const double seconds = std::chrono::duration<double>(now - start_).count();
        if (seconds < 4) return {};
        Report complete = window_;
        complete.seconds = seconds;
        window_ = {};
        start_ = now;
        return complete;
    }
private:
    FrameDrawTargets pending_;
    Report window_;
    Clock::time_point start_{};
    bool started_ = false;
};

} // namespace leonida
