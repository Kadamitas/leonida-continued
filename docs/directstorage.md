# DirectStorage

The console title reads its streamed data through Xbox DirectStorage: `DStorageGetFactory` hands out a factory, the
factory opens files and creates queues, and a queue reads file or memory ranges into memory and reports completion
through status arrays, `ID3D12Fence` signals and events. `runtime/shims/dstorage.cpp` serves that API on the PC:
files open through the runtime's `GAME:` mapping and write overlay, and a worker thread per queue does the reads
and inflates zlib requests.
This page records which DLL and ABI the console title is expected to use, how sure that is, and what the shim does.

## Which DLL and which ABI: the evidence

The game executable is not in the package available here, so its import table is unmeasured
([runtime_imports.md](runtime_imports.md)). What is known:

| Evidence | Source | What it says |
|---|---|---|
| None of the 16 shipped DLLs imports DirectStorage | [runtime_imports.md](runtime_imports.md) (6 Xbox-specific imports, all `xmem`/`d3d12_xs`), [textures.md](textures.md) "The shipped binaries" (no `DStorage` import, export or string) | DirectStorage, if used, is called by the executable itself |
| The texture chunks carry the `DSTORAGE_BCPACK_MODE` values 1-7 and mix zlib and BCPack chunks | [textures.md](textures.md) "What BCPack is" | The data is laid out for console DirectStorage's decompression (DEFLATE, then BCPack, then swizzle) |
| A sibling console layer's export inventory names `dstorage_xs.dll` as console DirectStorage | [d3d12_vulkan.md §7](d3d12_vulkan.md#7-prior-art-and-measured-xbox-abi) | The console module name, measured elsewhere, not here |
| A retail title run by a sibling layer has `xcrdapi` in its console-only library set | [d3d12_vulkan.md §7](d3d12_vulkan.md#7-prior-art-and-measured-xbox-abi) | Only the name. Nothing in this repository says what `xcrdapi.dll` exports |
| The GDK reference: `DStorageGetFactory` is in header `dstorage_xs.h`, library `dstorage_xs.lib`; the interface pages give library `dstorage.lib`; all say "Xbox Series consoles" | [GDK DStorage reference](https://learn.microsoft.com/en-us/gaming/gdk/docs/reference/system/dstorage/dstorage_members) | The console API has its own names (`IDStorageFactoryX`, `IDStorageQueueX`, `IDStorageFileX`, `IDStorageStatusArrayX`) and structures that differ from the PC API |
| `registry.cpp` classified `dstorage.dll`/`dstorage_xs.dll` `DStorageGetFactory` as returning an HRESULT | `runtime/shims/registry.cpp` | It was an unimplemented import; `xcrdapi.dll` was not named |

**Chosen ABI: the console one** (the GDK's `IDStorageFactoryX2`/`IDStorageQueueX2`/`IDStorageFileX`/
`IDStorageStatusArrayX` and its `DSTORAGE_REQUEST`), served as `DStorageGetFactory` under all three names:

| Module | Confidence | Why |
|---|---|---|
| `dstorage_xs.dll` | medium | The reference's `dstorage_xs.lib` and the sibling inventory agree; not measured in this title |
| `xcrdapi.dll` | low | Only a name in a library list. Owning the module also means a title's `LoadLibrary("xcrdapi.dll")` gets the shims' pseudo-handle, and `GetProcAddress` for any other name returns `NULL` (logged). Static imports of other `xcrdapi` names are unchanged |
| `dstorage.dll` | low | The interface pages name `dstorage.lib`. Every caller of `gta6rt`'s import table is a console build, so this name serves the console ABI too, not the PC one |

The PC DirectStorage ABI (`dstorage.h`: `IDStorageFactory` with `DSTORAGE_REQUEST` source and destination unions,
`ID3D12Resource` destinations) is **not served**. A caller that asks for a PC interface id gets `E_NOINTERFACE` and a
log line, never an object of the wrong shape. The one exception is `IDStorageStatusArray`, whose two methods are the
console interface's in the same order (console D3D12 binaries reuse the PC SDK's ids), so a status array accepts its
id.

Owning a module has a run-time side: `LoadLibrary` of `xcrdapi.dll` or `dstorage.dll` (or `dstorage_xs.dll`) gets
the shims' pseudo-handle even when the package ships its own copy of that DLL, so the shim shadows a packaged copy
at run time (`library_loader.cpp` asks the registry before the bundled loader). None of the 16 shipped DLLs is one
of these, so nothing is shadowed today.

### What is not public, and the choice made

| Unknown | Choice | Confidence | Where to change it |
|---|---|---|---|
| Vtable order of the console interfaces (the reference lists methods alphabetically) | The PC header's order for the methods both APIs have; console-only methods where the overview lists them: factory `CreateQueue, OpenFile, CreateStatusArray, SetCpuAffinity, SetDebugFlags, SetStagingBufferSize`, then `GetStats` (X1), `CreateQueue1` (X2); queue `EnqueueRequest, EnqueueStatus, EnqueueSignal, Submit, CancelRequestsWithTag, Close, GetErrorEvent, RetrieveErrorRecord, Query`, then `EnqueueSetEvent` (X1), `Query1` (X2); status array `IsComplete, GetHResult`; file `Close, GetHandle` | queue, status array: medium; file: medium; factory: low (the `SetCpuAffinity` slot is a guess) | `runtime/shims/dstorage_xs_abi.h`, one declaration per interface |
| Interface ids of the X interfaces | A creating call (`DStorageGetFactory`, `CreateQueue*`, `OpenFile`, `CreateStatusArray`) accepts an id it has not seen for the kind of object it hands out and logs it once (`[rt] dstorage: ... asked for {...}: serving the ... interface`), so the first boot records them. It refuses an id seen for another kind, a PC DirectStorage id, and the system COM interfaces (the `{xxxxxxxx-0000-0000-C000-000000000046}` range with `IMarshal`, `IClientSecurity`, ..., and `IAgileObject`, `INoMarshal`, `IInspectable` and a few more). `QueryInterface` follows [d3d12_vulkan.md](d3d12_vulkan.md)'s rule (an unknown id is `E_NOINTERFACE`, logged): it accepts only `IUnknown`, the declared `IDStorageStatusArray` id for a status array, and the ids a creating call accepted for that kind of object | n/a (a policy, not a fact) | `IidPolicy` in `runtime/shims/dstorage.cpp` |
| `DSTORAGE_MIN_QUEUE_CAPACITY`, `DSTORAGE_MAX_QUEUE_CAPACITY`, `DSTORAGE_REQUEST_MAX_NAME` | The PC header's values: `0x80`, `0x2000`, 64 | medium | `dstorage_xs_abi.h` |
| `E_DSTORAGE_*` values | The PC `dstorageerr.h` values (`FACILITY_GAME`, `0x8924xxxx`). That header also defines the console-only failures (zlib, BCPack, swizzle, XVD), so the space is shared. `E_DSTORAGE_DECOMPRESSION_ERROR` (`0x89240030`) is on the public constants page; `E_DSTORAGE_ZLIB_BAD_HEADER`, `_ZLIB_BAD_DATA` and `_ZLIB_PARITY_FAIL` (`0x89240031`-`33`) have no public description, and their names and values come from the header-generated bindings of the `direct-storage` crate (Sources). Which failure the console reports with each zlib code is inferred from the names | medium-high (values); medium (zlib codes' meaning) | `dstorage_xs_abi.h` |
| Struct layouts | The compiler's x64 layout of the reference's declarations, pinned with `static_assert` (`DSTORAGE_REQUEST` 72 bytes, `DSTORAGE_QUEUE_DESC` 24, `DSTORAGE_QUEUE_DESC1` 32); `DSTORAGE_REQUEST_OPTIONS` is decoded from `AsUINT64` by explicit bit positions (zlib bit 0, BCPack 1-4, swizzle 5-20, destination pages 21, source type 22, source pages 23, reserved 24-63) | high for member order and types, medium for the bit allocation (MSVC allocates bit-fields from bit 0 up) | `dstorage_xs_abi.h` |

The first boot with the executable settles most of this: the dry-run report shows which module it imports
`DStorageGetFactory` from, and the log lines give the interface ids.

## What the shim does

- **Factory**: one object for the process, never freed (its reference count does not drop below one).
  `SetDebugFlags` and `SetCpuAffinity` are stored only (a console core mask means nothing on the host).
  `SetStagingBufferSize` accepts 0, 16, 20, 24, 28 and 32 MiB, fails with `E_DSTORAGE_INVALID_STAGING_BUFFER_SIZE`
  otherwise and with `E_DSTORAGE_STAGING_BUFFER_LOCKED` while a queue or file exists; there is no staging buffer.
  `GetStats` reports the counts the shim keeps (queues per priority, requests, bytes, files and errors).
  `TotalDecompressionRequestCount` grows once when a zlib and/or BCPack request starts decoding, after its source
  was read; a combined request counts once. `PendingDecompressionRequestCount` counts requests inside decoding,
  and `DecompressionErrors` counts decode failures, including unsupported history and allocation failure during
  decoding. Source-read/allocation and final destination-write failures do not increment that last count.
  Scope guards balance the pending counts when allocation throws. As
  `DSTORAGE_PRIORITY_STATS` defines them, `TotalRequestCount` and the byte totals grow when a request is submitted
  (not when it is enqueued), and `PendingRequestCount` counts the requests the worker is processing; status and fence
  entries are counted when they are enqueued. A bad
  argument (a null pointer, an invalid queue desc or staging size) is logged once per method and kind, so a wrong
  vtable guess shows up in the log as arguments that make no sense for the method.
- **Files**: `OpenFile` goes through the registry's `kernel32.dll!CreateFileW`, the shim in `file_system.cpp`, so a
  `GAME:` path reads the overlay copy first, then the package, and a path that climbs out is refused (a host path is
  passed through). Read access, `FILE_SHARE_READ`, as the overview says; a failed open returns the Win32 error as an
  HRESULT. `GetHandle` returns a handle of its own (`ReOpenFile`, read access, `FILE_SHARE_READ`) that the caller
  closes; it has its own file position, which the shim's reads do not move. `Close` waits for the submitted requests
  that read the file; a request processed after it fails with `E_DSTORAGE_FILE_NOT_OPEN`. A request holds a reference
  on its file, so the last `Release` cannot close a file a request still reads.
- **Queues**: capacity `0x80`..`0x2000`, priority `LOW`..`REALTIME`, a memory-sourced queue must be `REALTIME`
  (`E_DSTORAGE_INVALID_QUEUE_CAPACITY`, `_INVALID_QUEUE_PRIORITY`, `_INVALID_MEMORY_QUEUE_PRIORITY`). A queue
  holds `capacity - 1` entries; `Query` reports the empty slots and the entries left before the automatic submit,
  which happens when the entries enqueued since the last submit reach half the capacity (unless
  `DSTORAGE_QUEUE_OPTIONS::AutoSubmitDisabled`). An enqueue into a full queue waits for space, as the reference
  says; with auto-submit disabled only a `Submit` from another thread frees it. One worker thread per queue runs
  the submitted entries in order, so every notification follows all the entries before it.
- **Requests**: copied at enqueue. A file or memory source is read into owned staging, followed by optional zlib
  inflation and strict BCPack decoding, then one final destination write. Overlapping source/destination buffers
  are safe, and failed decoding/allocation does not touch the destination. Checked in this order on the worker:
  1. cancelled: a success, whatever else is wrong with it;
  2. found at enqueue: a null request pointer (`E_INVALIDARG`) or a file object the shim did not make
     (`E_DSTORAGE_INVALID_FILE_HANDLE`);
  3. reserved option bits (`E_DSTORAGE_RESERVED_FIELDS`), then the source type against the queue's, or a
     `SourceIsPhysicalPages` flag on a file source (`E_DSTORAGE_INVALID_SOURCE_TYPE`);
  4. BCPack mode above 7, swizzle mode above 4 (`_INVALID_BCPACK_MODE`, `_INVALID_SWIZZLE_MODE`), then
     BC2 or nonzero swizzle, with or without zlib (`E_NOTIMPL`, logged once);
  5. a missing `IntermediateSize` for zlib + BCPack, or a nonzero one otherwise (`_INVALID_INTERMEDIATE_SIZE`),
     then the shim's explicit 32 MiB intermediate-staging limit (`_REQUEST_TOO_LARGE`). A `DestinationSize` other
     than `SourceSize` without decompression, or not larger with either kind of decompression, is
     `_INVALID_DESTINATION_SIZE`. The total source (memory requests only) plus destination must not exceed 1 GiB.
     Each ordinary address buffer retains the 32 MiB limit because its page size is unknown. A physical-page
     source or destination explicitly uses 64 KiB pages and can exceed that limit. Either zlib or BCPack requires
     a 16-byte-aligned file offset (`_INVALID_FILE_OFFSET`,
     as `DSTORAGE_REQUEST` requires);
  6. a null destination, a null source (memory) or a null file (`E_INVALIDARG`), a closed file (`_FILE_NOT_OPEN`);
  7. a zero size: a success that touches no memory (an assumption: the reference does not say);
  8. a range past the end of the file (`E_DSTORAGE_END_OF_FILE`), then a destination or source the process cannot
     access (`E_DSTORAGE_ACCESS_VIOLATION`; the worker survives it). This needs asynchronous exceptions under
     clang-cl, so `runtime/CMakeLists.txt` builds `dstorage.cpp` with `/EHa` there; MSVC always catches it.

  Step 7 does not apply to a decompression request (its `DestinationSize` is never 0 after step 5); its stream fails as
  described next.
- **zlib** (`ZlibDecompress` alone): the source, `SourceSize` bytes of the file or of memory, is read into a staging
  buffer and inflated there by the runtime's RFC 1950 inflater (`leonida::zlib_inflate` in
  `runtime/src/shader_library.h`, the one the shader library uses for XBCH blobs). The result is copied to
  `Destination` only when it is exactly `DestinationSize` bytes, so a stream that fails leaves the destination as it
  was, and a memory source inside its own destination (in-place loading) inflates correctly. Address destinations
  retain the 32 MiB limit; physical-page destinations use the 1 GiB request bound (step 5). Step 5 also means a
  stream of stored blocks only, which is larger than what it holds, is
  refused with `_INVALID_DESTINATION_SIZE` before it is read, as the documented rule implies. The stream is a zlib
  stream: the two header bytes, the deflate blocks (stored, fixed and
  dynamic Huffman), the Adler-32 of the result. Bytes after the Adler-32 are not read, so a `SourceSize` rounded up
  over padding works (an assumption: the reference does not say). The game's own data agrees with RFC 1950: the
  texture pools' codec-8 chunks, laid out for this decompression ([textures.md](textures.md)), inflate with
  Python's `zlib.decompress`, which checks the header and the Adler-32 (`tools/ttpf_mips.py`). A failure:

  | Stream | HRESULT |
  |---|---|
  | header not deflate, a window over 32 KiB, a wrong check value, or a preset dictionary | `E_DSTORAGE_ZLIB_BAD_HEADER` (`0x89240031`) |
  | a reserved block type, an invalid code, a distance before the start, or a truncated stream | `E_DSTORAGE_ZLIB_BAD_DATA` (`0x89240032`) |
  | Adler-32 mismatch | `E_DSTORAGE_ZLIB_PARITY_FAIL` (`0x89240033`) |
  | inflates to more or fewer bytes than `DestinationSize` | `E_DSTORAGE_DECOMPRESSION_ERROR` (`0x89240030`; no code names a size mismatch, so the generic one) |

  The inflater decodes one bit at a time, on the queue's worker: correct, not fast. A faster decoder (table lookup)
  can replace it behind `zlib_inflate` without touching the request path.
- **Strict BCPack**: `runtime/src/bcpack.cpp` reuses the dependency-free C++ decoder from
  [#4921](https://github.com/ItsNotRexed/Leonida/pull/4921), pinned at
  `86ab19058032fcd640423699529086168ad1e4f1`, with the structural progress rejection from
  [#7230](https://github.com/ItsNotRexed/Leonida/pull/7230). `tools/gen_bcpack_tables.py` regenerates its codec tables
  from the current Python sources. Modes 1, 3, 4 and 5 support self-contained prediction and repository-confirmed
  initial history; modes 6 and 7 support self-contained prediction, with confirmed BC7 signatures. BC2, unknown or
  provisional histories/signatures and external-history loading remain unsupported. Runtime calls never enable
  provisional decoding. Native/Python parity and hand-built block tests confirm this subset; console equivalence
  remains unverified.

  With both zlib and BCPack, `IntermediateSize` is exactly the zlib output and BCPack input size. It may be smaller
  than `SourceSize` (a stored zlib stream has header overhead); the final `DestinationSize` must still be larger.
  The 32 MiB intermediate cap is this CPU shim's conservative support limit, not a console ABI limit. A malformed
  BCPack stream returns the documented generic `E_DSTORAGE_DECOMPRESSION_ERROR`; unsupported capabilities/history
  return `E_NOTIMPL`. Both `std::bad_alloc` and `std::length_error` become `E_OUTOFMEMORY` on the worker.

  All decode results stay in owned staging until success. An address destination's complete committed/writable
  range is checked before copying, and physical pages are all validated before writing through their backing.
  The caller must keep its buffers and page arrays stable through completion; this does not serialize unrelated
  guest threads that change address protections.
- **Cancel**: `CancelRequestsWithTag(mask, value)` marks every request enqueued before the call and not yet started
  whose `(CancellationTag & mask) == value`. A cancelled request completes as a success without reading and is not
  counted as a failure, as the overview and the reference say, even when it also had an error found at enqueue.
- **Notifications**: `EnqueueStatus` sets the entry to `E_PENDING` at once and, when reached, to the first failure
  since the previous status or signal (else `S_OK`); `IsComplete` is `GetHResult != E_PENDING`; an index past the
  array is `E_DSTORAGE_INDEX_BOUND`, a foreign or null array `E_DSTORAGE_INVALID_STATUS_ARRAY`. `EnqueueSignal` calls
  `ID3D12Fence::Signal` (PC slot 10, which the `d3d12_xs` fence proxies forward) on the worker thread; a null fence is
  `E_DSTORAGE_INVALID_FENCE`. `EnqueueSetEvent` duplicates the handle at once and sets the duplicate, then closes it,
  so an event the title closes in the meantime (and a handle value reused for another object) is never the one set; a
  null or invalid handle is recorded as a failure. Each notification holds a reference on its array or fence until it
  has run.
- **Errors**: a failure goes into the status entry, the queue's error record (failure count and the first failure
  since the last `RetrieveErrorRecord`, with the file name the title opened) and sets the error event.
  `GetErrorEvent` returns a duplicate handle (the reference says the caller closes it). `RetrieveErrorRecord`
  clears the record and resets the event; with nothing recorded it returns `DSTORAGE_COMMAND_TYPE_NONE`. A failed
  `SetEvent` is recorded with command type 3 (the PC API's `EVENT`; the console enum stops at 2).
- **Close and release**: `Close` submits what is enqueued, waits until the worker has run it all and stops the
  worker; later enqueues are dropped and recorded as `E_DSTORAGE_QUEUE_CLOSED`. The last `Release` closes the same
  way, so a queue released with requests in flight finishes them first.

## Not done yet (later PRs)

- **Remaining BCPack capabilities**: BC2, unknown/provisional initial history, inferred signatures and native
  external-history loading. The strict supported subset is above.
- **MDU swizzle** (`SwizzleMode`): the post-BCPack macro-unshuffle remains `E_NOTIMPL`. AMD GPU texture-addressing
  equations perform a different operation. Microsoft's public texture-shuffling sample contains the later GPU
  micro-unshuffle and calls the console API for offline shuffling; it does not provide a CPU MDU implementation.
- **GPU memory**: the console API has no GPU destinations of its own (memory is unified, so a destination is an
  address). An address inside memory the Vulkan layer backs will need its write routed to that memory. The destination
  is written by `write_destination` after the ordered staging/decode path, so that change stays inside the request
  path.
- **Parallel reads**: the worker reads one request at a time. Overlapped reads with in-order retirement of
  notifications would keep the order guarantee.

## Physical-page destinations

A request with `DestinationIsPhysicalPages` has no destination address. The union that holds `Destination` holds
`DestinationPageArray`, a pointer to page numbers, and `DestinationPageOffset`, a `UINT16`; the data starts that many
bytes into the first page and runs on through the following entries. The page numbers are the ones
`XMemAllocatePhysicalPages` hands out, 64 KiB each in the `xmem` shim ([runtime_imports.md](runtime_imports.md)), which
is also why the offset fits 16 bits.

The retail executable builds such requests. Read from its code, with nothing run: its chunked read path asks whether
the destination address lies inside the range it backs with physical pages, and if it does it passes the address of
that page's entry in its own page table as the array and the low 16 bits of the address as the offset, with the option
bit set; otherwise it passes the address itself. The same path takes `ZlibDecompress`, `BcpackMode` 1 to 7 and
`SwizzleMode` 1 to 4 from the chunk's entry in the file's chunk table, and never `SourceIsPhysicalPages`. How often
each combination occurs in a boot has not been counted (a trace of the first requests would give it).

What the shim does (`write_destination` in `shims/dstorage.cpp`, `rt::xmem::write_physical_pages` in
`shims/xmem_physical.cpp`, adapted from upstream #7652 to this page store):

- the source (a file range or memory) is read into a staging buffer and, for a zlib request, inflated there, as for
  an address;
- the page numbers the range covers, `(DestinationPageOffset + size)` rounded up to whole pages, are copied out of
  the title's array under the same guard as every other read of title memory;
- every page number is looked up before a byte is written, so an unreadable array, page number 0 (which the map calls
  read as "unmap"), a number that was never handed out and a freed page all fail the request with
  `E_DSTORAGE_ACCESS_VIOLATION`, the code of a destination that cannot be written, and leave the pages as they were;
- the bytes are written through the page store's permanent writable backing views. Validation and copying hold
  the page-store lock, preventing free/reuse between them; no temporary mapping or allocation can fail after
  validation. A page needs no guest mapping to be written, and all its mapped aliases show the bytes at once.

`DSTORAGE_MAX_4K_PAGE_DESTINATION_SIZE` (32 MiB) does not apply to these requests: the reference allows a larger
buffer with 64 KiB or 2 MiB pages, and physical pages are 64 KiB. An address keeps the limit, because the shim cannot
tell the page size behind it. **Unverified:** the console's failure code for a bad page number (the header has none
that names one), and whether the console requires the pages to be mapped.

## Physical-page sources

`SourceIsPhysicalPages` is supported on memory-sourced queues. `SourcePageArray` names the same 64 KiB physical
pages and `SourcePageOffset` is a 16-bit offset into the first page. A file-source request with this flag fails
with `E_DSTORAGE_INVALID_SOURCE_TYPE`; it never interprets a file object as a page array.

The worker first allocates its owned source buffer, then copies the required page IDs out of the caller's array
under the guarded memory-read helper. `rt::xmem::read_physical_pages` validates every ID under the page-store lock
before copying any bytes from the permanent backing views into that buffer. It holds the lock through the copy,
so an allocation/free operation cannot recycle a page midway through the snapshot. Guest mappings are unnecessary;
reads observe bytes written through any CPU alias. The caller keeps the array, allocated pages and source bytes
stable through completion.

The snapshot then enters the existing zlib/strict-BCPack stages and single final destination write. Thus physical
source/destination overlap uses the original source bytes, and source-array, allocation or decode failures leave
the destination unchanged. A zero-sized raw request does not read a nonnull array, even if it is inaccessible;
null source pointers remain `E_INVALIDARG`. Invalid, zero or freed IDs and unreadable arrays fail with
`E_DSTORAGE_ACCESS_VIOLATION`. The exact console failure code for invalid IDs remains unverified.

Each address-based source or destination is conservatively capped at 32 MiB. Explicit physical-page buffers can
exceed 32 MiB, with source plus destination bounded by the existing 1 GiB memory-request limit. A synthetic file
source still allows a larger physical destination; an ordinary memory-address source no longer inherits that
exception merely because its destination uses pages.

## Tests

`runtime/test/dstorage_test.cpp` (ctest `dstorage`) runs on synthetic files in `%TEMP%` through the registry entries
under all three module names: `GAME:` reads with an overlay copy winning, offsets, zero size, a range past the end,
cancellation, auto-submit, a full queue, status arrays, fences (a CPU fence object that checks the data is in place
when it is signalled), events, error records, file and queue close with requests in flight, a destination the
process cannot write, four threads enqueueing at once, the interface-id policy (unknown and system ids refused by
`QueryInterface`, learned by creating calls), a `GetHandle` handle with its own file position, an event closed (and
its handle value reused) before it is set, and cancelled requests that also had an enqueue error. zlib requests use
synthetic streams only: two made with Python's `zlib` (level 9: one dynamic-Huffman block; level 1: one
fixed-Huffman block) from text the test rebuilds, and stored-block streams the test builds, one of them a stored
block followed by the dynamic one. They are read from a file and from memory (also in place); a bad header, a
preset dictionary, a reserved block type, a truncated stream, an Adler-32 mismatch, a stream shorter and one longer
than `DestinationSize`, a stored stream larger than its result, an unaligned offset, an `IntermediateSize`, zlib with
BCPack or swizzle (`E_NOTIMPL`), an unreadable source and an unwritable destination each get their code, a failed
stream leaves its destination untouched, and the decompression statistics count them. Physical-page destinations use
pages from the `xmem` shim's own exports (two allocations with pages supplied out of order; the separate 513-page
case spans multiple backing chunks)
and read every result back through a window the pages are mapped into: a file read that starts inside one page and
ends in the fourth, zlib from memory across a page boundary, a copy that ends exactly at the end of a page, pages that
were mapped during the request and pages mapped only afterwards, a request over 32 MiB, and the failures (a number that
is not a live page, page number 0, a freed page, an unreadable array), which must leave every page untouched. A
physical-page source uses the same backing store, as described above.

Physical-source queue tests cover mapped, multiply aliased and unmapped pages, reordered arrays and offset 65535,
source/destination overlap, zlib and strict BCPack, zero size, dead/invalid/unreadable arrays, allocation/length
failure and exact statistics. A complete copy over 32 MiB uses unmapped physical sources and physical destinations;
oversized address buffers and source-plus-destination sums over 1 GiB fail before reading arrays or source bytes.
The `xmem_physical` test also verifies the helper itself leaves its staging prefix and canaries untouched when a
later page ID is invalid, and rejects overflow before any array access.

Strict BCPack queue tests use 512 repeated hand-built BC1 blocks encoded by the repository's synthetic encoder,
with an independently specified expected block payload. File, memory, overlapping in-place, zlib + BCPack
(including `IntermediateSize < SourceSize`) and physical-page destinations reproduce every byte. Malformed
entropy, wrong decoded sizes, invalid intermediate sizes, the explicit intermediate cap, unsupported BC2/swizzle,
unconfirmed/unknown histories, alignment, unreadable source/page arrays and mixed-protection destinations are
refused without partial destination writes. Test-only injection covers allocation and length failures at source,
inflate, native output and physical-page-array staging, including the zlib-only path; exact decompression and
request counts remain balanced. No real memory exhaustion is required.

`ctest -R "^(bcpack|bcpack_cpp_vs_python|dstorage)$"` covers the native self-checks, native/Python differential
comparison and queue behavior. These synthetic/model checks do not establish console BCPack equivalence.

## Sources and licences

| Source | Facts used | Licence / use |
|---|---|---|
| [GDK DStorage reference](https://learn.microsoft.com/en-us/gaming/gdk/docs/reference/system/dstorage/dstorage_members) and its function, interface, structure and enumeration pages | Names, method signatures, structure members, enum values, requirements rows | [Microsoft Learn Terms of Use](https://learn.microsoft.com/en-us/legal/termsofuse); facts paraphrased, no text copied |
| [DirectStorage overview (GDK)](https://learn.microsoft.com/en-us/gaming/gdk/docs/features/console/storage/directstorage/directstorage-overview) | Queue, auto-submit, cancellation, notification order, file sharing, decode order | Same terms; paraphrased |
| `dstorage.h`, `dstorageerr.h` in the public `Microsoft.Direct3D.DirectStorage` 1.3.0 NuGet package | PC method order, interface ids (to refuse them), capacity and name-length constants, `E_DSTORAGE_*` values | Read for facts only; not vendored. `runtime/shims/dstorage_xs_abi.h` is Leonida's own declaration |
| [DirectStorage constants](https://learn.microsoft.com/en-us/windows/win32/dstorage/dstorage-constants) (Win32 reference) | `E_DSTORAGE_DECOMPRESSION_ERROR` = `0x89240030`; `E_DSTORAGE_INVALID_DESTINATION_SIZE`: equal to the length without decompression, larger with it; `E_DSTORAGE_INVALID_FILE_OFFSET` is the offset invalid for a decompression mode | [Microsoft Learn Terms of Use](https://learn.microsoft.com/en-us/legal/termsofuse); paraphrased |
| [`direct-storage` crate](https://docs.rs/direct-storage/0.5.1/src/direct_storage/bindings.rs.html) 0.5.1 (bindings generated by `windows-bindgen` from the DirectStorage headers; 0.2.0's `errors.rs` has the same values) | `E_DSTORAGE_ZLIB_BAD_HEADER` `0x89240031`, `E_DSTORAGE_ZLIB_BAD_DATA` `0x89240032`, `E_DSTORAGE_ZLIB_PARITY_FAIL` `0x89240033` (names and values only) | MIT/Apache-2.0; two constants' names and values, no code copied |
| [RFC 1950](https://www.rfc-editor.org/rfc/rfc1950) and [RFC 1951](https://www.rfc-editor.org/rfc/rfc1951) | The zlib stream and DEFLATE formats `zlib_inflate` decodes | Public specifications |
| [Leonida #4921](https://github.com/ItsNotRexed/Leonida/pull/4921), head `86ab19058032fcd640423699529086168ad1e4f1`, and [#7230](https://github.com/ItsNotRexed/Leonida/pull/7230), head `469eb386be4195267f045ff0ac297db2cf753ef8` | Native strict codec, generated tables, differential harness and precise malformed-fragment rejection | Same repository GPL-3.0 licence; upstream authors retained in commit attribution; tables regenerated from current codec sources |
