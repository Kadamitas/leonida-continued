# XG import bridge to the isolated texture core

`LEONIDA_TEXTURE_CORE_DIR` explicitly selects the separately built `leonida-texture-core` project, source commit `ce1ffc77098cc8686752552f5bce38b91bf3414d`. Its four `lxg_*` C exports wrap real CPU texture layout/copy operations. Source/license provenance and verified hashes are preserved there: 22 upstream files pinned to [omonkeyz/xbox-emulator f89e4106fe8a1728515afa34ceb5097132cadcda](https://github.com/omonkeyz/xbox-emulator/tree/f89e4106fe8a1728515afa34ceb5097132cadcda/crates/compat/xg_xs), plus the [PR292](https://github.com/omonkeyz/xbox-emulator/pull/292) suggestion implementation. The core is GPL-3.0-or-later and retains original notices; it has inert shared logging and no TomBox initialization or console-DLL forwarding.

The two runtime bindings are grounded in the exact executable SHA-256 `e95020b2fa4a721db3df59fdecf75621730ac559d8835252f6c3b658d5a08993`, inspected statically without execution:

| Import | Observed contract | Call-site RVAs |
|---|---|---|
| XGCreateTextureComputer | RCX points to a 56-byte D3D12_RESOURCE_DESC prefix; RDX points to an object output. Two sites check negative HRESULT; two immediately use the output object. | `0x344235`, `0x979c75`, `0x16f7141`, `0x5121a5e` |
| XGSuggestSwizzleMode | RCX descriptor, EDX flags 0, R8 DWORD output aliasing descriptor layout at `+0x2c`; EAX is passed to the title's signed-error helper. | `0xc4b3ce`, `0x4eccca2` |

The bridge first copies exactly 56 guest bytes into a local descriptor through a narrow SEH guard. Rust receives only local descriptor/output pointers. Returned pointers/DWORDs are published through the same guard; failed object publication releases the newly created native object through its real Release slot. The descriptor copy precedes publishing the suggestion, preserving the observed output/input alias. Backend suggestion failures leave guest output unchanged; inaccessible publication returns `E_POINTER`.

The returned object is the backend's actual native address computer. Observed title uses include Release slot 1; resource-layout slot 2 (output `0x1b40`, plane stride `0x6c8`, mip stride `0x70`); seven-argument copy slot 8; and seven-argument tile-metadata slot 10. Slot 10's output parameter shapes match public D3D12 tile metadata, including three 12-byte structures. Its Xbox return type remains unestablished because the observed caller ignores it. The backend's integer result reports unsupported cases without inventing output.

Tile metadata currently supports validated single-sample, single-plane 2D 64KiB layouts, with reverse-mip/tail offsets preserved. Linear, 4KiB, VAR, multi-plane/CMASK, unsupported 1D/3D or otherwise unverified layouts fail before any output writes. The title's ignored result does not make those paths supported. Other object methods retain the documented upstream support limits.

No hardware SKU is inferred. The backend retains upstream default profile 0; the numerical relationship between hardware profiles 0/4 and this title's console SKU is unestablished. This bridge neither forces profile 4 nor changes the default based on the DLL or game name. The PR292 swizzle algorithm reuses existing native layout rules but lacks an independent console-oracle comparison for this export.

Build the separate core, then configure this runtime with an absolute `-DLEONIDA_TEXTURE_CORE_DIR=.../leonida-texture-core`. CMake imports its release DLL/import library and copies the DLL beside `gta6rt` and `texture_core_bridge_test`; no global PATH change is needed. Keep the backend loaded while any returned object remains alive. The object's method buffers still follow its caller-owned valid-span contract; this bridge guards the two import boundaries, not every native vtable method.

The focused bridge test uses the real registry and backend for descriptor/layout forwarding, CPU texture copy roundtrip, independently typed seven-argument tile metadata, output aliasing, unsupported flags/layouts, exact reads at a guard-page boundary and inaccessible/read-only outputs. On Windows/MSVC 19.36 with SDK 10.0.26100.0, CTest `texture_core_bridge` passed 63 checks with zero failures. Core verification separately passed 27 native-engine tests, two Rust FFI tests and one native C/DLL test. These checks do not establish a console hardware profile or complete title texture compatibility.
