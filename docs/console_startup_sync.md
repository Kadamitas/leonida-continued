# Observed console startup synchronization

The newer command-list layout has bottom-of-pipe writes at slots 71 and 72. A successful checked ResetX at slot
139 identifies that layout. These adapters do not apply to the older layout.

Slot 71 accepts an aligned 32-bit destination with zero flush and flag modifiers. It records a real
`WriteImmediate` marker-out operation. Slot 72 accepts an aligned 64-bit destination, value zero, flush zero and
GPU-timestamp flag 1. It records a native Vulkan timestamp and a 64-bit query-result copy to the destination.
Microsoft's [public timer sample](https://github.com/microsoft/Xbox-GDK-Samples/blob/b1e77b6ac10f95b07296025fda171ca2ddc42dbe/Kits/ATGTK/PerformanceTimersXbox.cpp)
demonstrates the timestamp convention. Literal 64-bit and system-clock writes remain unsupported.

Destination pages must be committed write-back memory importable as HOST_VISIBLE | HOST_COHERENT. The list and
each submitted batch retain the actual imported backing. The device's page cache holds weak references to avoid
a device/resource ownership cycle. Marker stores run on the GPU; timestamp query pools are private to each
execution and survive until its submission retires. Neither operation writes an early CPU completion value.
The immediate queue path retains external page owners as well as the path held behind fence waits.

With `LEONIDA_XBOX_CACHE_SYNC=conservative`, one observed 64 KiB XMem allocation tuple is allocated write-back
instead of write-combined: null address, allocation type 0x20401000, attributes 0x283 and protection 0x42404.
This changes the cache modifier of a new allocation only. Existing pages are not reprotected, and arbitrary
write-combined imports remain refused.

`LEONIDA_XBOX_MEMORY_WAIT=satisfied_host_generation` enables a bounded startup acquire for slot 74:
GREATER_EQUAL, reference 1, flags DWORD 1, and the observed CPU control-page role. The CPU initializes this cell
to 1 before publishing it; reviewed later publications remain positive. An actual coherent 64-bit read during
every replay must satisfy the predicate. Replay then records a HOST_WRITE-to-ALL_COMMANDS memory barrier at
the command's position. An unsatisfied value fails preparation rather than being overwritten or signalled.
Reference 2 can follow a prefix of immediate memory writes disjoint from the control cell. The queue submits
preceding complete lists and the verified write prefix, then waits for their real Vulkan progress ticket without
holding the device's resource or submission locks. At that boundary a fresh unsigned comparison consumes a CPU
publication that may subsequently regress. The suffix is prepared and submitted only on success. A failure after
prefix submission stops the guest device and reports partial execution. Multiple later waits, other mid-list
prefixes and unsatisfied values remain unsupported. The remaining console flag-1 semantics are unverified.

The separate `LEONIDA_XBOX_QUEUE_SUBMIT=immediate` experiment issues a real PC ExecuteCommandLists after
validating and retaining the complete console list array. Console token pacing remains unverified. Recording
success, issuing that call, successful native submission, GPU completion, and rendered gameplay are distinct
outcomes; none should be inferred from an earlier one. Actual launch probes have observed native timeline
completion of startup marker, timestamp and clear submissions. Playable output remains unestablished.

Device frame-interval, origin scheduling and frame-wait calls now use the same clock and pacer as PresentX.
The peer-derived flag-2 timeout path returns genuine S_FALSE with a tracked token, without reporting an origin
event that was not observed. Queue35 validates the same issued-token sequence. Unknown flags and ancillary
pointers remain refused. PresentX borrows an unambiguous process-owned Americas window; it does not destroy it
or request activation. An owned fallback stays hidden, and presentation is destroyed before its window.

## Native color target allocation

The `observed_color` resource profile also admits single-sample 2D render targets in A8_UNORM and B8G8R8A8_UNORM
with exact console flags 0x300001, and an R16G16B16A16_FLOAT render/UAV target with flags 0x300005. These have one
layer and mip, UNKNOWN layout and default alignment. Both allocation-info-2 and title-address creation preserve
the RT/UAV capability flags and check the native format's capabilities. The observed FP16 target additionally
accepts initial UNORDERED_ACCESS layout3; the other admitted color targets retain SHADER_RESOURCE layout6.
The allocation
query returns real Vulkan requirements; it does not guess a console metadata reservation. Native alpha render
targets require A8 format support and never substitute an R8 render target. CPU access and raw storage aliases
remain unsupported.
