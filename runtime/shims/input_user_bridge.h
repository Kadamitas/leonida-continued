// Optional bridge between the SDL GameInput shim and the one offline XUser. No SDL dependency: builds without
// the input shim have no device provider and cannot claim that an arbitrary device is paired.
#pragma once

#include <array>
#include <atomic>
#include <vector>

namespace rt::input_user {

using DeviceId = std::array<unsigned char, 32>;
using DeviceSnapshot = std::vector<DeviceId>;
using DeviceProvider = DeviceSnapshot (*)();
using ChangeListener = bool (*)();

inline std::atomic<DeviceProvider> device_provider{nullptr};
inline std::atomic<ChangeListener> change_listener{nullptr};
inline std::atomic<bool> user_added{false};
inline std::atomic<bool> refresh_requested{false};

inline DeviceSnapshot connected_devices() {
    const auto provider = device_provider.load(std::memory_order_acquire);
    return provider ? provider() : DeviceSnapshot{};
}

// Call only outside GameInput's state/poll locks: a task queue can dispatch a notification immediately and the
// title can re-enter input or user APIs. The listener contains its own allocation boundary; guest callbacks are
// deliberately not enclosed in a catch-all that could hide a title exception.
inline void retry_changes() {
    if (!refresh_requested.exchange(false, std::memory_order_acq_rel)) return;
    const auto listener = change_listener.load(std::memory_order_acquire);
    if (!listener || !listener()) refresh_requested.store(true, std::memory_order_release);
}

inline void notify_changed() {
    refresh_requested.store(true, std::memory_order_release);
    retry_changes();
}

// Collecting a successful XUserAddAsync result makes the offline user visible to the title. A failed/cancelled add
// cannot reach that result provider. Closing a duplicate handle is not sign-out.
inline void added_to_title() {
    if (!user_added.exchange(true, std::memory_order_acq_rel)) notify_changed();
}

}  // namespace rt::input_user
