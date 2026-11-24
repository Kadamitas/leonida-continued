// The seam between xmem.cpp's XMemVirtualAlloc and the physical-page store in xmem_physical.cpp.
//
// xmem.cpp is compiled into many test targets that do not link xmem_physical.cpp, so it does not call into it
// directly: xmem_physical.cpp registers its reservation function here at static initialisation, and XMemVirtualAlloc
// uses it when it is set. Without it (a target that does not link the store), the reservation is an ordinary one.
#pragma once
#include <windows.h>

namespace rt::xmem {

// Reserves `size` bytes (a multiple of 64 KiB) of address space that XMemMapPhysicalPages[Scatter] can map pages
// into, with `protect` as the CPU protection of every page mapped there. Null with the last error set on failure.
using PhysicalReserveFn = void* (*)(SIZE_T size, DWORD protect);

void set_physical_reserve(PhysicalReserveFn fn);
PhysicalReserveFn physical_reserve();

// Explicit placeholder creation and reserve-only physical subranges. The store
// retains the parent reservation so selecting a subrange cannot discard its
// neighbours or release address space between selection and page mapping.
using PlaceholderReserveFn = void* (*)(void* address, SIZE_T size, DWORD allocation_type, DWORD protect);
void set_placeholder_reserve(PlaceholderReserveFn fn);
PlaceholderReserveFn placeholder_reserve();

#ifdef LEONIDA_XMEM_TEST_HOOKS
// One deterministic native failure, compiled only into the synthetic CPU test.
void fail_physical_unmap_once_for_test(void* address);
void fail_physical_release_once_for_test(DWORD free_type);
#endif

}  // namespace rt::xmem
