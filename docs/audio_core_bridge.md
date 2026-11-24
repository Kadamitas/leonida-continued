# CPU audio core and measured import bridge

`LEONIDA_AUDIO_CORE_DIR` explicitly selects a separately built, source-pinned CPU audio backend. When set, the runtime binds 19 AcpHal/XAPU/DSP imports to actual heap, codec, queue and DSP processing. This does not connect a playback device or establish full title audio compatibility. When unset, none of these bindings is registered.

The backend is the independent local `leonida-audio-core` project, source commit `aa47e76074f022d3bc7a6c60226c232d0dd6eb66`, derived from [omonkeyz/xbox-emulator f89e4106fe8a1728515afa34ceb5097132cadcda](https://github.com/omonkeyz/xbox-emulator/tree/f89e4106fe8a1728515afa34ceb5097132cadcda). Its `vendor/` contains 58 verified upstream source/license files, `verified-sources.json` records hashes, and `prepare_engines.py` reproduces the narrow adaptations. GPL-3.0-or-later applies to the compatibility engines/adapter, LGPL-2.1-or-later to XMA, and BSD-3-Clause to Opus; original license/notices are retained. Stock TomBox DLLs are not loaded. The isolated backend has no TomBox allocator, GDK/OS/input/network initialization, or recording activation.

## Static evidence and scope

The following contracts were checked against the local executable with SHA-256 `e95020b2fa4a721db3df59fdecf75621730ac559d8835252f6c3b658d5a08993`, using PE import thunks, `.pdata` boundaries, direct call/tail-jump scans and targeted LLVM disassembly. REA independently identified the same PE/SHA, without deep analysis or execution. Counts cover the direct sites found by `tools/import_call_sites.py`; unresolved register-indirect uses are not excluded. No target was executed for this analysis. This table records semantic observations and locations, not game code or data.

| Import | Direct sites | Observed argument/result contract and evidence RVA |
|---|---:|---|
| ApuCreateHeap | 1 | Two DWORD sizes in RCX/RDX; second `0x226e00`; negative HRESULT checked. `0x43ad933`. |
| ApuAlloc | 1 | RCX pointer output, RDX optional APU output null, R8 byte size, R9 alignment `0x800`, fifth stack argument heap selector. Wrapper `0x43a6110`; callers use selectors 1/2 and read the pointer output. |
| ApuFree | 1 tail jump | RCX pointer forwarded unchanged by `0x43aea10`; callers ignore the return. The backend's integer error return is preserved, with no stronger claim about the title's unused return type. |
| ApuIsVirtualAddressValid | 4 | RCX CPU pointer, EDX alignment `0x40`, `0x80`, or `0x800`; result tested in AL. `0x78790`, `0x787e3`, `0x78afd`, `0x78b41`. |
| ApuMapVirtualAddress | 4 | Only RCX is explicitly set at each call; returned EAX stored as 32-bit APU address. `0x7879c`, `0x787ef`, `0x78b09`, `0x78b4d`. RDX residue from the previous validation call does not establish a second argument. |
| ApuMapApuAddress | 2 | ECX 32-bit APU address; returned RAX used as pointer/null. `0x783f6`, `0x78485`. |
| AcpHalAllocateShapeContexts | 1 | RCX input/output structure; six count DWORDs, six CPU pointers at `+0x18`, six APU DWORDs at `+0x48`; total `0x60`. The caller reads XMA CPU pointer at `+0x38`. `0x43aec73`. |
| AcpHalReleaseShapeContexts | 1 | No argument explicitly established at `0x43ae899`; RCX is volatile residue from a preceding virtual release. The backend's zero-argument function matches. Return ignored. |
| AcpHalCreate | 1 | RCX pointer output, negative HRESULT checked, result object immediately called through its vtable. `0x43aed98`. No second argument is established. |
| XApuConnect | 1 | RCX six-DWORD input configuration, RDX 16-byte base/event output, R8 handle output; negative HRESULT checked. `0x43aee69`; constructor `0x5a87480` initializes the input as codec 1, 256 streams, depth 2, memory `0x104000`, frame 0, options 1. |
| XApuDisconnect | 1 | RCX handle, then caller clears its global; no second argument established. `0x43ae930`. |
| XApuEnqueueCommand | 4 | RCX handle, RDX packed command, R8 optional sequence output (all observed sites pass null). Types 1, 2, 3, 5; data command has pointer/length pairs at `+0x28/+0x2c` and `+0x34/+0x38`. `0x483d5`, `0x4ee9978`, `0x4f2092d`, `0x4f20bb8`. |
| XApuGetResultCount | 1 | RCX handle, RDX optional dequeued count null; EAX controls result-dequeue loop. `0x4eaa86b`. |
| XApuDequeueResult | 1 | RCX handle, RDX result output; caller reserves at least `0x50`, reads type/stream and result fields, tests HRESULT negative. `0x4eaa89a`. |
| XDspConnectWithMaximumStreamLimit | 1 | Six arguments: workspace, DWORD bytes, activations, streams `0x80`, priority 1, client output pointer; return zero means success. `0x5b5262e`. |
| XDspActivate | 1 | Four import arguments: client, descriptor, status-pointer output, context output. Outer title wrapper `0x11d4720` has five title arguments, promotes its fifth to a register; inner `0x11d4750` swaps output registers. Callers `0x3b97e` and `0x43b3966` confirm descriptor and distinct outputs. No fifth argument is forwarded to the import. |
| XDspSubmitCommand | 1 | Three import arguments: context, 32-byte command, sequence DWORD output. Wrapper `0x4e5b200` swaps RCX/RDX and preserves R8. Six wrapper callers set R8 to a DWORD output and subsequently read it, including `0x43b2d45` and `0x4e5af74`. |
| XDspDeactivate | 1 tail jump | RCX context forwarded unchanged through `0x1166010/0x1166020`; caller `0x1165fd7` clears its context/status fields afterward. Return unused. |
| XDspDisconnect | 1 tail jump | RCX client forwarded through `0x43ae9c0/0x43aea00`; caller `0x43ae96b` then releases workspace. Return unused. |

The earlier `audio_hal_exe_calls.md` was a deliberately incomplete first pass. In particular, its XApuConnect input/output ambiguity, two-argument DSP submit estimate and alleged extra argument residues are resolved above.

## HAL vtable boundary

The returned proxy's six engine methods follow the pinned backend: slot 0 connects command/message rings, 1 disconnects, 2 submits supported SHAPE commands, 3 pops a 28-byte message, 4 counts messages, and 5 releases. Exact title sites confirm slots 0/2 at `0x43aedba/0x43aee2f`, 1/5 at `0x43ae84f/0x43ae88b`, and additional slot-2 commands at `0x78521`, `0x92619` and `0x43a7416`. Slots 3/4 are genuine backend queue operations; no additional title call site is claimed here. Slots 6 through 11 remain unmeasured for this title and terminate with a diagnostic if called, rather than returning invented output or success. Unsupported command types within measured slot 2 return `E_NOTIMPL` from the backend.

The proxy uses backend tokens internally and never interprets title pointers as Rust objects. XAPU command packing is copied with unaligned-safe loads; short control commands are copied only through their used fields into a zeroed local command. DSP status pointers borrow the backend's actual four-DWORD status array until deactivation. Impulse-response input is copied at activation and may be outside client workspace; processing buffers and multipliers must fit the registered workspace. Caller buffers must remain valid until asynchronous work completes.

## Building and verification

Build the separate core first (`build.ps1` there), then configure this runtime with an absolute `-DLEONIDA_AUDIO_CORE_DIR=.../leonida-audio-core`. CMake imports its release DLL/import library, includes its C header, and copies the DLL beside both `gta6rt` and `audio_core_bridge_test`. No global PATH change is required. Keep the DLL loaded for process lifetime after SHAPE contexts start its service thread.

The separate core passed 56 synthetic Rust tests plus its native C/DLL test; 14 external-reference tests were explicitly filtered because fixtures are absent. The runtime bridge test drives all 19 imports through the real registry/backend, checks APU/context layouts, real HAL messages, the measured XAPU configuration, asynchronous PCM output and queues, FFT numerical roundtrip, external convolution input, output guards and stale-handle failure. On Windows/MSVC 19.36 with SDK 10.0.26100.0, both `gta6rt` and `audio_core_bridge_test` built successfully; CTest `audio_core_bridge` passed 1,637 checks with zero failures. The copied backend DLL's SHA-256 matched its built artifact. This validation does not claim a playable title or working output device.
