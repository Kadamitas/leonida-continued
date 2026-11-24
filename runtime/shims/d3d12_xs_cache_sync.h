// Opt-in synchronization experiment for two unmodelled console list slots.
//
// The accepted cache masks match AMD PAL's GFX10 ACQUIRE_MEM GCR_CNTL fields; the
// other control bits and the slot-to-API identities are NOT established. This
// deliberately records a conservative, real memory dependency as a hypothesis. It
// is not an exact implementation of either console operation. Disabled and
// unsupported requests return false so the caller retains its strict path.
#pragma once

#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace rt::d3d12_xs {
namespace cache_sync_detail {

inline bool enabled() {
    // Process policy: set before launch. Exact value only; no implicit enabling
    // by ordinary unimplemented-call or tracing settings.
    static const bool value = [] {
        char setting[13]{};
        return GetEnvironmentVariableA("LEONIDA_XBOX_CACHE_SYNC", setting, sizeof(setting)) == 12 &&
               std::strcmp(setting, "conservative") == 0;
    }();
    return value;
}

inline bool observed_full_range_tuple(unsigned slot, std::uint64_t a1, std::uint64_t a2,
                                      std::uint64_t a3, std::uint64_t a4) {
    if (a3 != 0 || a4 != UINT64_C(0x0000ffffffffff00)) return false;
    if (slot == 131)
        return ((a1 == 0xc280 || a1 == 0xc380) && a2 == 0x20000) ||
               (a1 == 0xc3c0 && a2 == 0x80020000);
    return slot == 132 && (a1 == 0x100000 || a1 == 0x700000) && a2 == 0;
}

}  // namespace cache_sync_detail

// true means the real GCL7 Barrier call was issued, not that Close/submission
// succeeded or that the console flag interpretation has been verified. No
// output pointers or arbitrary ranges are accepted. The caller keeps its raw
// slot/argument trace and must keep normal backend Close/submission checking.
inline bool try_record_conservative_cache_sync(ID3D12GraphicsCommandList* real, unsigned slot,
                                               std::uint64_t a1, std::uint64_t a2,
                                               std::uint64_t a3, std::uint64_t a4) {
    if (!real || !cache_sync_detail::enabled() ||
        !cache_sync_detail::observed_full_range_tuple(slot, a1, a2, a3, a4))
        return false;
    const auto type = real->GetType();
    if (type != D3D12_COMMAND_LIST_TYPE_DIRECT && type != D3D12_COMMAND_LIST_TYPE_COMPUTE) return false;

    // QI can return an adjusted interface pointer. Use that pointer for Barrier
    // and retain it until the call has returned; never index the base vtable.
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList7> enhanced;
    if (FAILED(real->QueryInterface(IID_PPV_ARGS(enhanced.GetAddressOf()))) || !enhanced) return false;

    // Interface availability does not imply that the device supports enhanced
    // barriers. Unsupported foreign backends retain the strict fallback too.
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    if (FAILED(real->GetDevice(IID_PPV_ARGS(device.GetAddressOf()))) || !device) return false;
    D3D12_FEATURE_DATA_D3D12_OPTIONS12 options{};
    if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS12, &options, sizeof(options))) ||
        !options.EnhancedBarriersSupported)
        return false;

    D3D12_GLOBAL_BARRIER barrier{};
    barrier.SyncBefore = D3D12_BARRIER_SYNC_ALL;
    barrier.SyncAfter = D3D12_BARRIER_SYNC_ALL;
    barrier.AccessBefore = D3D12_BARRIER_ACCESS_COMMON;
    barrier.AccessAfter = D3D12_BARRIER_ACCESS_COMMON;
    D3D12_BARRIER_GROUP group{};
    group.Type = D3D12_BARRIER_TYPE_GLOBAL;
    group.NumBarriers = 1;
    group.pGlobalBarriers = &barrier;
    enhanced->Barrier(1, &group);

    static std::atomic<bool> announced[2]{};
    if (!announced[slot - 131].exchange(true, std::memory_order_relaxed))
        std::fprintf(stderr,
                     "[rt] d3d12_xs: EXPERIMENTAL synchronization hypothesis for list slot %u: "
                     "issued real GCL7 ALL/COMMON global barrier; console flag interpretation UNVERIFIED "
                     "(LEONIDA_XBOX_CACHE_SYNC=conservative)\n",
                     slot);
    return true;
}

}  // namespace rt::d3d12_xs
