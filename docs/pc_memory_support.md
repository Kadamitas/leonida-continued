# Local PC memory support

This local integration combines the physical-page work in upstream PRs
[#6995](https://github.com/ItsNotRexed/Leonida/pull/6995) and
[#7331](https://github.com/ItsNotRexed/Leonida/pull/7331) with a narrow part of
[#6919](https://github.com/ItsNotRexed/Leonida/pull/6919): the default allocator exports, the tools-memory query,
and ordinary host backing for the console's 2 MiB virtual-page mode. It does not establish that the game boots.

## Heap allocations

`XMemAlloc` and `XMemAllocDefault` share one CRT aligned-heap helper. `XMemFree` and `XMemFreeDefault` can release
blocks from either entry point. Each wrapper records its own API name and caller in the optional startup trace.

The public measurements in [#3601](https://github.com/ItsNotRexed/Leonida/pull/3601) identify the alignment exponent
in bits 24-28 and page-size code 1 in bits 14-15 as 64 KiB. The helper honors exponents through 21, with a minimum
of 64 bytes, and raises the alignment to 64 KiB for page-size code 1. The attribute word `0xf0814000` consequently
gets 64 KiB alignment. This adds heap padding; it does not allocate dedicated console pages. Larger exponents fail
with null and `ERROR_NOT_SUPPORTED`, avoiding both GiB-sized padding and a successful underaligned pointer. Other page codes and memory-type
bits are not interpreted. Zero-size heap allocations retain the existing one-writable-byte policy.

`XMemIsToolsMemAvailable` returns false: a desktop process has no separate development-kit tools-memory pool.

## Virtual allocations

`XMemVirtualAlloc` accepts the measured nonzero access words without claiming to interpret their memory-type or
coherence fields. The existing conversion of console `PAGE_GRAPHICS_*` bits to CPU protection remains in place.

The console mode `0x20000000` uses normal host pages, with a 2 MiB-aligned base and sizes rounded up to 2 MiB.
New reservations use dynamically resolved `VirtualAlloc2` and `MEM_ADDRESS_REQUIREMENTS.Alignment`; the guest flag
is never forwarded as host `MEM_LARGE_PAGES`, so lock-memory privilege is unnecessary. If that API is unavailable,
the new-reservation request explicitly fails with `ERROR_NOT_SUPPORTED`. An existing-address commit expands to the
whole guest page it touches. Overflow and invalid page-mode combinations fail before a host allocation occurs.

The physical store retains its earlier hook for reserve-only 64 KiB windows. This addition does not extend physical
page tokens to the 2 MiB mode, emulate GPU coherence, remember a reservation's guest page mode for later calls that
omit it, or add 4 MiB page support.

Explicit `MEM_RESERVE | MEM_RESERVE_PLACEHOLDER` creation is also supported for the measured null-address,
`PAGE_NOACCESS` form. A reserve-only 64 KiB `MEM_REPLACE_PLACEHOLDER` selection inside that parent updates the
selected pages' CPU protection without discarding neighbouring windows or releasing address space. The store owns
the whole parent until its release; physical mappings split its placeholders lazily. Live mappings and out-of-parent
selections fail before metadata changes. Private committed replacements remain unsupported.

Protections belong to individual pages, so independently selected windows retain their CPU access and write-combining
policy during mapping and rollback. The synthetic test exercises all five protection words recorded in
`docs/xmem_exe_calls.md`, including the direct `MapViewOfFile3` write-combined path.

## Verification without game files

`xmem_test` resolves all functions through the runtime's shim registry. Its synthetic checks cover cross-freeing,
zero-byte and impossible heap requests, observed alignment words, distinct wrapper tracing, 2 MiB reservation and
commit rounding, partial commits, graphics-protection conversion, integer overflow and invalid page modes.
`xmem_physical_test` separately covers the physical store and rollback behavior. Neither test needs game files.
