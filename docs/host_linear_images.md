# CPU-owned linear images

`rt::d3d12::create_readonly_texture_at_host_address` imports the caller's actual committed memory into a
linear `VkImage` with `VK_EXT_external_memory_host`. It takes an explicit accessible byte count and row pitch.
It allocates no substitute GPU pixel buffer. A one-time backup preserves the initial imported allocation around
the first legal layout transition; later CPU updates are read directly from the imported memory. The texture reports GPU VA zero,
as D3D12 textures do; its descriptor key identifies the image independently of CPU addresses.

The first subset is `TEXTURE2D`, `ROW_MAJOR`, `R8G8B8A8_UNORM`, one mip, one slice, one sample, and no resource
flags. The address is 64 KiB aligned. The entire imported, rounded span must be committed ordinary read/write
memory. Only a host-visible, host-coherent Vulkan memory type is accepted. Unsupported formats, GPU-write
flags, missing import support, driver layout mismatches and unavailable memory types return failure.

`ROW_MAJOR` does not fix a row pitch: Microsoft's public definition permits padding. The helper accepts an
image only if `vkGetImageSubresourceLayout` reports offset zero and exactly the supplied row pitch. A zero
pitch is allowed only for a single-row image, which has no inter-row padding to infer. The existing slot-133
host-address handler uses that exception only for single-row textures. It continues to refuse multirow and
tiled console textures; this change establishes no relationship between a console layout word and an XG
swizzle mode or hardware profile.

On the RTX 5080 with Windows driver 32.0.16.2124, a synthetic RGBA8 linear-image probe returned pitches 32,
256, 256 and 288 for widths 4, 63, 64 and 65. All had offset zero. Those differing answers are why the helper
checks rather than infers pitch. This is a driver observation, not a portable layout rule.

## Submission and lifetime

The caller retains the memory through final `Release`, creates no other GPU resource alias of that physical
span, and fences earlier GPU reads before changing its bytes. Distinct CPU mappings are permitted; a generic
host-pointer API cannot infer whether two virtual pointers share physical pages.
Ordinary CPU writes completed before submission are visible to the next submission; non-temporal stores need
their usual host ordering. Every backend execution records a `HOST_WRITE`/`HOST` to memory-read barrier for
live imported images. External-memory images must be created in `UNDEFINED`, and host-pointer imports cannot
use a dedicated-image allocation chain. The constructor backs up the entire rounded imported span, creates
and binds a non-dedicated import, synchronously transitions `UNDEFINED` to `GENERAL` and waits, then restores
that entire span before publishing the resource. This preserves initial pixels and padding without assuming
that an undefined-layout transition retains memory contents. Images requiring dedicated allocations are refused.
Subsequent uses remain in `GENERAL`, where direct CPU writes are legal. A restore is never delayed until the
first caller submission, which would overwrite updates made after creation. Other queues wait for initial
layout transitions through the backend's initialization semaphore tracking.

The image has sampled and transfer-source usage only. Replay refuses copy destinations, clears, draw
attachments, resolves, discards, enhanced texture barriers, and alias barriers involving the import. A global
alias scope is refused while imports exist. UAV and RTV creation do not produce writable views of these
images. `WriteToSubresource` returns `E_NOTIMPL`; the caller writes its own memory directly.

Final release first detaches the binding so concurrent unrelated submissions cannot capture another automatic
image barrier, joins earlier CPU submissions and waits their GPU work, removes descriptor entries, replaces bindless slots with fallbacks, and
destroys views and the image before freeing imported Vulkan memory. The caller's pages are never freed by
the resource. This does not implement GPU image aliasing; the tested CPU aliases are two host mappings of
one pagefile-backed section.

## Synthetic checks

`host_linear_texture_test` uses original GLSL/SPIR-V, no game shaders. It samples the imported image on the
GPU before any image copy, then checks later copies and samples after CPU writes made after command
recording, including writes through a second CPU mapping. Widths 63 and 64 exercise the exact 256-byte-pitch
case. It checks invalid spans/layouts and pairs replay refusals with ordinary-image positive controls.
The test also checks full initial-span/padding preservation and the single-row host-address route. Vulkan
validation remains optional; a run without the Khronos layer establishes output comparisons, not validation
cleanliness. Validation found and corrected the initial `PREINITIALIZED`/dedicated-chain approach; the legal
construction follows VUIDs `VkImageCreateInfo-pNext-01443` and `VkMemoryAllocateInfo-pNext-02806`.

The existing buffer import from [PR #7218](https://github.com/ItsNotRexed/Leonida/pull/7218) supplies the
extension/lifetime pattern. [PR #7411](https://github.com/ItsNotRexed/Leonida/pull/7411) explicitly leaves
texture hook memory as bookkeeping. [PR #770](https://github.com/ItsNotRexed/Leonida/pull/770) concerns
ordinary PC placed-image aliasing, rather than the byte layout of title-owned CPU memory. None was blindly
merged to claim this path.

References: [D3D12 texture layouts](https://learn.microsoft.com/windows/win32/api/d3d12/ne-d3d12-d3d12_texture_layout),
[host pointer imports](https://docs.vulkan.org/refpages/latest/refpages/source/VkImportMemoryHostPointerInfoEXT.html),
[linear image layouts](https://docs.vulkan.org/refpages/latest/refpages/source/vkGetImageSubresourceLayout.html),
[host write ordering](https://docs.vulkan.org/spec/latest/chapters/synchronization.html#synchronization-submission-host-writes).
