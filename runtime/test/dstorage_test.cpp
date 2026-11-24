// DirectStorage shim (runtime/shims/dstorage.cpp): DStorageGetFactory resolved through the shim registry under its
// three module names, driven the way a console title calls it, over synthetic files in %TEMP%. No game data, no GPU.
#include <windows.h>
#include <d3d12.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "dstorage_xs_abi.h"
#include "bcpack_fixture.h"
#include "file_system.h"
#include "shims.h"

using namespace dsx;
namespace fs = std::filesystem;

namespace {

int g_checks = 0;
int g_failures = 0;

void check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("FAIL: %s\n", what);
    }
}

void check_hr(HRESULT got, HRESULT want, const char* what) {
    ++g_checks;
    if (got != want) {
        ++g_failures;
        std::printf("FAIL: %s (got 0x%08lx, want 0x%08lx)\n", what, static_cast<unsigned long>(got),
                    static_cast<unsigned long>(want));
    }
}

// Interface ids a title might pass. The console's own ids are not public, so these are made up; the shim accepts any
// id per kind of object.
constexpr GUID kFactoryId = {0x0d5e0001, 0x1111, 0x4111, {0x81, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x01}};
constexpr GUID kQueueId = {0x0d5e0002, 0x1111, 0x4111, {0x81, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x02}};
constexpr GUID kFileId = {0x0d5e0003, 0x1111, 0x4111, {0x81, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x03}};
constexpr GUID kStatusId = {0x0d5e0004, 0x1111, 0x4111, {0x81, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x04}};
// IDStorageFactory of the PC API (dstorage.h): refused, its object has another shape.
constexpr GUID kPcFactoryId = {0x6924ea0c, 0xc3cd, 0x4826, {0xb1, 0x0a, 0xf6, 0x4f, 0x4e, 0xd9, 0x27, 0xc1}};

// A CPU fence with the PC ID3D12Fence vtable (the one the d3d12_xs fence proxies forward). It records the values it is
// signalled with and runs a check at each signal.
class TestFence final : public ID3D12Fence {
public:
    std::function<void(UINT64)> on_signal;
    std::atomic<UINT64> value{0};
    std::atomic<ULONG> refs{1};

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (riid != __uuidof(IUnknown) && riid != __uuidof(ID3D12Fence)) return E_NOINTERFACE;
        AddRef();
        *ppv = this;
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override { return --refs; }  // lives on the test's stack
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT*, void*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetName(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID, void** ppv) override {
        if (ppv) *ppv = nullptr;
        return E_NOTIMPL;
    }
    UINT64 STDMETHODCALLTYPE GetCompletedValue() override { return value.load(); }
    HRESULT STDMETHODCALLTYPE SetEventOnCompletion(UINT64, HANDLE) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Signal(UINT64 v) override {
        if (on_signal) on_signal(v);
        value.store(v);
        return S_OK;
    }
};

std::vector<uint8_t> pattern(size_t n, uint32_t seed) {
    std::vector<uint8_t> v(n);
    uint32_t x = seed * 2654435761u + 1;
    for (auto& b : v) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        b = static_cast<uint8_t>(x);
    }
    return v;
}

void write_file(const fs::path& p, const std::vector<uint8_t>& bytes) {
    fs::create_directories(p.parent_path());
    FILE* f = nullptr;
    if (_wfopen_s(&f, p.c_str(), L"wb") != 0 || !f) {
        std::printf("FAIL: cannot write %ls\n", p.c_str());
        std::exit(1);
    }
    if (!bytes.empty()) std::fwrite(bytes.data(), 1, bytes.size(), f);
    std::fclose(f);
}

bool wait_complete(IDStorageStatusArrayX* s, UINT32 index, int ms = 10000) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (!s->IsComplete(index)) {
        if (std::chrono::steady_clock::now() > end) return false;
        Sleep(1);
    }
    return true;
}

bool wait_until(const std::function<bool()>& done, int ms = 10000) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (!done()) {
        if (std::chrono::steady_clock::now() > end) return false;
        Sleep(1);
    }
    return true;
}

DSTORAGE_REQUEST file_request(IDStorageFileX* f, uint64_t offset, void* dst, uint32_t n, uint64_t tag = 0,
                              const char* name = nullptr) {
    DSTORAGE_REQUEST r{};
    r.File = f;
    r.FileOffset = offset;
    r.Destination = dst;
    r.DestinationSize = n;
    r.SourceSize = n;
    r.CancellationTag = tag;
    r.Name = name;
    return r;
}

struct Env {
    fs::path root, game, overlay;
    IDStorageFactoryX2* factory = nullptr;
    std::vector<uint8_t> a_package, a_overlay, b, big;
};

IDStorageQueueX2* make_queue(Env& env, uint16_t capacity = 0x80, bool auto_submit = true,
                             DSTORAGE_REQUEST_SOURCE_TYPE type = DSTORAGE_REQUEST_SOURCE_FILE,
                             DSTORAGE_PRIORITY priority = DSTORAGE_PRIORITY_NORMAL, const char* name = "test queue") {
    DSTORAGE_QUEUE_DESC1 d{};
    d.SourceType = type;
    d.Capacity = capacity;
    d.Priority = priority;
    d.Name = name;
    d.Options.AutoSubmitDisabled = auto_submit ? 0 : 1;
    void* q = nullptr;
    const HRESULT hr = env.factory->CreateQueue1(&d, kQueueId, &q);
    check_hr(hr, S_OK, "CreateQueue1");
    return static_cast<IDStorageQueueX2*>(q);
}

IDStorageFileX* open(Env& env, const wchar_t* path) {
    void* f = nullptr;
    check_hr(env.factory->OpenFile(path, kFileId, &f), S_OK, "OpenFile");
    return static_cast<IDStorageFileX*>(f);
}

IDStorageStatusArrayX* make_status(Env& env, UINT32 capacity) {
    void* s = nullptr;
    check_hr(env.factory->CreateStatusArray(capacity, "test status", kStatusId, &s), S_OK, "CreateStatusArray");
    return static_cast<IDStorageStatusArrayX*>(s);
}

// ---- tests ----------------------------------------------------------------------------------------------------------

void test_layout_and_options() {
    DSTORAGE_REQUEST_OPTIONS o{};
    o.ZlibDecompress = 1;
    check(o.AsUINT64 == options::kZlib, "ZlibDecompress is bit 0");
    o.AsUINT64 = 0;
    o.BcpackMode = DSTORAGE_BCPACK_MODE_BC7;
    check(options::bcpack(o.AsUINT64) == 7 && o.AsUINT64 == (7ull << options::kBcpackShift), "BcpackMode is bits 1-4");
    o.AsUINT64 = 0;
    o.SwizzleMode = DSTORAGE_SWIZZLE_MODE_4K_12K_4K_12K;
    check(options::swizzle(o.AsUINT64) == 4 && o.AsUINT64 == (4ull << options::kSwizzleShift),
          "SwizzleMode is bits 5-20");
    o.AsUINT64 = 0;
    o.DestinationIsPhysicalPages = 1;
    check(o.AsUINT64 == options::kDestinationPages, "DestinationIsPhysicalPages is bit 21");
    o.AsUINT64 = 0;
    o.SourceType = DSTORAGE_REQUEST_SOURCE_MEMORY;
    check(o.AsUINT64 == options::kSourceMemory, "SourceType is bit 22");
    o.AsUINT64 = 0;
    o.SourceIsPhysicalPages = 1;
    check(o.AsUINT64 == options::kSourcePages, "SourceIsPhysicalPages is bit 23");
    o.AsUINT64 = 0;
    o.Reserved = 1;
    check(o.AsUINT64 == (1ull << 24) && (o.AsUINT64 & options::kReservedMask), "Reserved starts at bit 24");
    DSTORAGE_QUEUE_OPTIONS q{};
    q.AutoSubmitDisabled = 1;
    check(q.AsUINT64 == 1, "AutoSubmitDisabled is bit 0");
}

void test_registry(Env& env) {
    const char* names[] = {"dstorage_xs.dll", "xcrdapi.dll", "dstorage.dll"};
    void* first = rt::shims::find("dstorage_xs.dll", "DStorageGetFactory");
    check(first != nullptr, "dstorage_xs.dll!DStorageGetFactory is registered");
    for (const char* n : names) {
        check(rt::shims::find(n, "DStorageGetFactory") == first, "every module name serves the same function");
        check(rt::shims::owns_dll(n), "the shims own the module (LoadLibrary gets a pseudo-handle)");
    }
    auto get = reinterpret_cast<DStorageGetFactoryFn>(first);
    if (!get) std::exit(1);

    check_hr(get(kFactoryId, nullptr), E_POINTER, "DStorageGetFactory without ppv");
    void* pc = reinterpret_cast<void*>(1);
    check_hr(get(kPcFactoryId, &pc), E_NOINTERFACE, "the PC IDStorageFactory id is refused");
    check(pc == nullptr, "a refused factory clears ppv");

    void* f1 = nullptr;
    void* f2 = nullptr;
    check_hr(get(kFactoryId, &f1), S_OK, "DStorageGetFactory");
    check_hr(reinterpret_cast<DStorageGetFactoryFn>(rt::shims::find("xcrdapi.dll", "DStorageGetFactory"))(
                 __uuidof(IUnknown), &f2),
             S_OK, "DStorageGetFactory(IUnknown) under xcrdapi.dll");
    check(f1 && f1 == f2, "the factory is one object");
    env.factory = static_cast<IDStorageFactoryX2*>(f1);
    static_cast<IUnknown*>(f2)->Release();
    check(env.factory->Release() >= 1 && env.factory->Release() >= 1, "the factory's count does not drop below one");
    env.factory->AddRef();

    void* again = nullptr;
    check_hr(env.factory->QueryInterface(kFactoryId, &again), S_OK, "factory QueryInterface for its own id");
    if (again) static_cast<IUnknown*>(again)->Release();
}

void test_factory_checks(Env& env) {
    // Before any queue or file exists.
    check_hr(env.factory->SetStagingBufferSize(17u << 20), E_DSTORAGE_INVALID_STAGING_BUFFER_SIZE, "staging 17 MiB");
    check_hr(env.factory->SetStagingBufferSize(16u << 20), S_OK, "staging 16 MiB");
    check_hr(env.factory->SetStagingBufferSize(0), S_OK, "staging 0");
    check_hr(env.factory->SetCpuAffinity(0x3c), S_OK, "SetCpuAffinity");
    env.factory->SetDebugFlags(DSTORAGE_DEBUG_SHOW_ERRORS);

    auto create = [&](DSTORAGE_REQUEST_SOURCE_TYPE type, uint16_t cap, int prio, uint64_t opts = 0) {
        DSTORAGE_QUEUE_DESC1 d{};
        d.SourceType = type;
        d.Capacity = cap;
        d.Priority = static_cast<DSTORAGE_PRIORITY>(prio);
        d.Options.AsUINT64 = opts;
        void* q = reinterpret_cast<void*>(1);
        const HRESULT hr = env.factory->CreateQueue1(&d, kQueueId, &q);
        if (SUCCEEDED(hr)) static_cast<IUnknown*>(q)->Release();
        else check(q == nullptr, "a failed CreateQueue1 clears ppv");
        return hr;
    };
    check_hr(create(DSTORAGE_REQUEST_SOURCE_FILE, 0x7f, 0), E_DSTORAGE_INVALID_QUEUE_CAPACITY, "capacity 0x7f");
    check_hr(create(DSTORAGE_REQUEST_SOURCE_FILE, 0x2001, 0), E_DSTORAGE_INVALID_QUEUE_CAPACITY, "capacity 0x2001");
    check_hr(create(DSTORAGE_REQUEST_SOURCE_FILE, 0x80, 3), E_DSTORAGE_INVALID_QUEUE_PRIORITY, "priority 3");
    check_hr(create(DSTORAGE_REQUEST_SOURCE_FILE, 0x80, -2), E_DSTORAGE_INVALID_QUEUE_PRIORITY, "priority -2");
    check_hr(create(DSTORAGE_REQUEST_SOURCE_MEMORY, 0x80, 0), E_DSTORAGE_INVALID_MEMORY_QUEUE_PRIORITY,
             "a memory queue must be realtime");
    check_hr(create(static_cast<DSTORAGE_REQUEST_SOURCE_TYPE>(2), 0x80, 0), E_DSTORAGE_INVALID_SOURCE_TYPE,
             "source type 2");
    check_hr(create(DSTORAGE_REQUEST_SOURCE_FILE, 0x80, 0, 2), E_DSTORAGE_RESERVED_FIELDS, "queue option bit 1");
    check_hr(create(DSTORAGE_REQUEST_SOURCE_FILE, 0x2000, -1), S_OK, "capacity 0x2000, low priority");
    check_hr(create(DSTORAGE_REQUEST_SOURCE_MEMORY, 0x80, 2), S_OK, "a realtime memory queue");
    check_hr(env.factory->CreateQueue1(nullptr, kQueueId, nullptr), E_POINTER, "CreateQueue1 without ppv");
    void* q = nullptr;
    check_hr(env.factory->CreateQueue(nullptr, kQueueId, &q), E_INVALIDARG, "CreateQueue without a desc");
    check_hr(env.factory->CreateQueue1(nullptr, kQueueId, &q), E_INVALIDARG, "CreateQueue1 without a desc");
    void* s = nullptr;
    check_hr(env.factory->CreateStatusArray(0, nullptr, kStatusId, &s), E_INVALIDARG, "a status array of 0");

    // The id policy: an id seen for a queue is not a factory id.
    void* wrong = nullptr;
    check_hr(env.factory->QueryInterface(kQueueId, &wrong), E_NOINTERFACE, "a queue id is not a factory id");
    check_hr(env.factory->CreateStatusArray(4, nullptr, kFactoryId, &s), E_NOINTERFACE,
             "a factory id is not a status array id");

    // CreateQueue (DSTORAGE_QUEUE_DESC) and Query reflect the desc.
    DSTORAGE_QUEUE_DESC d{};
    d.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
    d.Capacity = 0x100;
    d.Priority = DSTORAGE_PRIORITY_HIGH;
    d.Name = "named";
    check_hr(env.factory->CreateQueue(&d, kQueueId, &q), S_OK, "CreateQueue");
    auto* queue = static_cast<IDStorageQueueX2*>(q);
    DSTORAGE_QUEUE_INFO info{};
    queue->Query(&info);
    check(info.Desc.Capacity == 0x100 && info.Desc.Priority == DSTORAGE_PRIORITY_HIGH && info.Desc.Name == d.Name &&
              info.Desc.SourceType == DSTORAGE_REQUEST_SOURCE_FILE,
          "Query returns the creation desc");
    check(info.EmptySlotCount == 0xff, "an empty queue has capacity - 1 empty slots");
    check(info.RequestCountUntilAutoSubmit == 0x80, "auto-submit at half the capacity");
    DSTORAGE_QUEUE_INFO1 info1{};
    queue->Query1(&info1);
    check(info1.Desc.Options.AsUINT64 == 0 && info1.EmptySlotCount == 0xff, "Query1");
    check_hr(env.factory->SetStagingBufferSize(32u << 20), E_DSTORAGE_STAGING_BUFFER_LOCKED,
             "staging size is locked while a queue exists");
    void* qi = nullptr;
    check_hr(queue->QueryInterface(kQueueId, &qi), S_OK, "queue QueryInterface");
    if (qi) static_cast<IUnknown*>(qi)->Release();
    check(queue->Release() == 0, "the last queue reference frees it");
    check_hr(env.factory->SetStagingBufferSize(32u << 20), S_OK, "staging size unlocked again");
}

void test_open_paths(Env& env) {
    void* f = reinterpret_cast<void*>(1);
    check_hr(env.factory->OpenFile(L"GAME:\\data\\missing.bin", kFileId, &f), HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND),
             "a missing GAME: file");
    check(f == nullptr, "a failed OpenFile clears ppv");
    check_hr(env.factory->OpenFile(L"GAME:\\..\\outside.bin", kFileId, &f), HRESULT_FROM_WIN32(ERROR_INVALID_NAME),
             "a GAME: path that climbs out is refused");
    check_hr(env.factory->OpenFile(nullptr, kFileId, &f), E_INVALIDARG, "OpenFile without a path");
    check_hr(env.factory->OpenFile(L"GAME:\\data\\a.bin", kFileId, nullptr), E_POINTER, "OpenFile without ppv");

    // The overlay copy wins over the package; a host path is passed through.
    IDStorageFileX* a = open(env, L"GAME:\\data\\a.bin");
    IDStorageFileX* b = open(env, L"game:/data/b.bin");
    const std::wstring host = (env.game / L"data" / L"b.bin").wstring();
    IDStorageFileX* h = open(env, host.c_str());
    if (!a || !b || !h) std::exit(1);

    IDStorageQueueX2* q = make_queue(env);
    IDStorageStatusArrayX* s = make_status(env, 4);
    std::vector<uint8_t> da(4096), db(1000), dh(1000);
    DSTORAGE_REQUEST r = file_request(a, 0, da.data(), 4096);
    q->EnqueueRequest(&r);
    r = file_request(b, 500, db.data(), 1000);
    q->EnqueueRequest(&r);
    r = file_request(h, 500, dh.data(), 1000);
    q->EnqueueRequest(&r);
    q->EnqueueStatus(s, 0);
    check_hr(s->GetHResult(0), E_PENDING, "a status entry is pending once enqueued");
    q->Submit();
    check(wait_complete(s, 0), "the reads complete");
    check_hr(s->GetHResult(0), S_OK, "the reads succeed");
    check(std::memcmp(da.data(), env.a_overlay.data(), 4096) == 0 && da != env.a_package,
          "GAME: reads the overlay copy, not the package");

    // GetHandle: a handle of its own (ReOpenFile), so its file position does not follow the shim's reads, which
    // just ended at byte 4096 of this file.
    HANDLE own = a->GetHandle();
    LARGE_INTEGER size{};
    check(own != INVALID_HANDLE_VALUE && GetFileSizeEx(own, &size) && size.QuadPart == 5000,
          "GetHandle names the overlay copy (5000 bytes, the package has 4096)");
    std::vector<uint8_t> via_handle(6000);
    DWORD got = 0;
    check(own != INVALID_HANDLE_VALUE && ReadFile(own, via_handle.data(), 6000, &got, nullptr) && got == 5000 &&
              std::memcmp(via_handle.data(), env.a_overlay.data(), 5000) == 0,
          "a ReadFile through GetHandle's handle starts at byte 0 and reads the overlay copy");
    if (own != INVALID_HANDLE_VALUE) CloseHandle(own);
    r = file_request(a, 100, da.data(), 50);
    q->EnqueueRequest(&r);
    q->EnqueueStatus(s, 1);
    q->Submit();
    check(wait_complete(s, 1) && std::memcmp(da.data(), env.a_overlay.data() + 100, 50) == 0,
          "the shim's reads do not follow a GetHandle handle's position either");
    check(std::memcmp(db.data(), env.b.data() + 500, 1000) == 0,
          "a GAME: read at an offset (forward slashes, any case)");
    check(db == dh, "a host path reads the same file");
    a->Release();
    b->Release();
    h->Release();
    s->Release();
    q->Release();
}

void test_reads_and_errors(Env& env) {
    IDStorageFileX* b = open(env, L"GAME:\\data\\b.bin");  // 10,000 bytes
    IDStorageFileX* big = open(env, L"GAME:\\data\\big.bin");
    IDStorageQueueX2* q = make_queue(env, 0x80, false);
    IDStorageStatusArrayX* s = make_status(env, 32);
    HANDLE error_event = q->GetErrorEvent();
    check(error_event != nullptr, "GetErrorEvent returns a handle");

    std::vector<uint8_t> big_out(env.big.size());
    std::vector<uint8_t> few(64, 0xAB);
    std::vector<uint8_t> zero(16, 0xCD);
    struct Case {
        DSTORAGE_REQUEST r;
        HRESULT want;
        const char* what;
    };
    std::vector<Case> cases;
    cases.push_back({file_request(big, 0, big_out.data(), static_cast<uint32_t>(big_out.size())), S_OK,
                     "a 3 MiB read"});
    cases.push_back({file_request(b, 10000, zero.data(), 0), S_OK, "a zero-size read at the end of the file"});
    cases.push_back({file_request(b, 9990, few.data(), 11, 0x77, "past-the-end"), E_DSTORAGE_END_OF_FILE,
                     "a read one byte past the end"});
    cases.push_back({file_request(b, ~0ull - 4, few.data(), 8), E_DSTORAGE_END_OF_FILE, "an offset near 2^64"});
    cases.push_back({file_request(b, 0, few.data(), 8), S_OK, "a good read after failures"});
    {
        DSTORAGE_REQUEST r = file_request(b, 0, few.data(), 8);
        r.DestinationSize = 16;
        cases.push_back({r, E_DSTORAGE_INVALID_DESTINATION_SIZE, "DestinationSize differs from SourceSize"});
        r = file_request(b, 0, few.data(), 8);
        r.Options.ZlibDecompress = 1;
        cases.push_back({r, E_DSTORAGE_INVALID_DESTINATION_SIZE, "zlib with DestinationSize equal to SourceSize"});
        r = file_request(b, 0, few.data(), 8);
        r.Options.BcpackMode = DSTORAGE_BCPACK_MODE_BC2;
        cases.push_back({r, E_NOTIMPL, "BC2 BCPack remains unsupported"});
        r = file_request(b, 0, few.data(), 8);
        r.Options.SwizzleMode = DSTORAGE_SWIZZLE_MODE_4K_4K_8K;
        cases.push_back({r, E_NOTIMPL, "swizzle is not implemented yet"});
        r = file_request(b, 0, few.data(), 8);
        r.Options.AsUINT64 = 8ull << options::kBcpackShift;
        cases.push_back({r, E_DSTORAGE_INVALID_BCPACK_MODE, "BCPack mode 8"});
        r = file_request(b, 0, few.data(), 8);
        r.Options.AsUINT64 = 5ull << options::kSwizzleShift;
        cases.push_back({r, E_DSTORAGE_INVALID_SWIZZLE_MODE, "swizzle mode 5"});
        r = file_request(b, 0, few.data(), 8);
        r.Options.Reserved = 1;
        cases.push_back({r, E_DSTORAGE_RESERVED_FIELDS, "a reserved option bit"});
        r = file_request(b, 0, few.data(), 8);
        r.Options.SourceIsPhysicalPages = 1;
        cases.push_back({r, E_DSTORAGE_INVALID_SOURCE_TYPE, "a FILE queue cannot use physical-page sources"});
        r = file_request(b, 0, few.data(), 8);
        r.IntermediateSize = 4;
        cases.push_back({r, E_DSTORAGE_INVALID_INTERMEDIATE_SIZE, "IntermediateSize without zlib and BCPack"});
        r = file_request(b, 0, nullptr, 8);
        cases.push_back({r, E_INVALIDARG, "a null destination"});
        r = file_request(nullptr, 0, few.data(), 8);
        cases.push_back({r, E_INVALIDARG, "a null file"});
        r = file_request(reinterpret_cast<IDStorageFileX*>(s), 0, few.data(), 8);
        cases.push_back({r, E_DSTORAGE_INVALID_FILE_HANDLE, "a status array passed as a file"});
        r = file_request(b, 0, few.data(), 8);
        r.Options.SourceType = DSTORAGE_REQUEST_SOURCE_MEMORY;
        cases.push_back({r, E_DSTORAGE_INVALID_SOURCE_TYPE, "a memory request on a file queue"});
    }
    cases.push_back({file_request(b, 0, nullptr, 0), E_INVALIDARG, "a zero-size read still needs a destination"});
    cases.push_back({file_request(nullptr, 0, zero.data(), 0), E_INVALIDARG, "a zero-size read still needs a file"});
    void* reserved = VirtualAlloc(nullptr, 1 << 16, MEM_RESERVE, PAGE_READWRITE);
    cases.push_back({file_request(b, 0, reserved, 4096), E_DSTORAGE_ACCESS_VIOLATION,
                     "a destination the process cannot write"});
    check(cases.size() <= 31, "the cases fit the status array");

    for (UINT32 i = 0; i < cases.size(); ++i) {
        q->EnqueueRequest(&cases[i].r);
        q->EnqueueStatus(s, i);
    }
    Sleep(30);
    check(!s->IsComplete(0), "nothing runs before Submit when auto-submit is disabled");
    DSTORAGE_QUEUE_INFO1 info{};
    q->Query1(&info);
    check(info.RequestCountUntilAutoSubmit == 0, "no auto-submit count when it is disabled");
    check(info.EmptySlotCount == 0x7f - 2 * cases.size(), "EmptySlotCount counts every entry");
    q->Submit();
    const UINT32 last = static_cast<UINT32>(cases.size() - 1);
    check(wait_complete(s, last), "all cases complete");
    for (UINT32 i = 0; i < cases.size(); ++i) check_hr(s->GetHResult(i), cases[i].want, cases[i].what);
    check(big_out == env.big, "the 3 MiB read has the file's bytes");
    check(zero == std::vector<uint8_t>(16, 0xCD), "a zero-size read writes nothing");
    check(std::memcmp(few.data(), env.b.data(), 8) == 0, "the good read after failures");

    check(WaitForSingleObject(error_event, 0) == WAIT_OBJECT_0, "the error event is set");
    DSTORAGE_ERROR_RECORD rec{};
    q->RetrieveErrorRecord(&rec);
    size_t failures = 0;
    for (const auto& c : cases) failures += FAILED(c.want) ? 1 : 0;
    check(rec.FailureCount == failures, "FailureCount counts every failed request");
    check_hr(rec.FirstFailure.HResult, E_DSTORAGE_END_OF_FILE, "the first failure is recorded");
    check(rec.FirstFailure.CommandType == DSTORAGE_COMMAND_TYPE_REQUEST, "its command type is REQUEST");
    check(rec.FirstFailure.Request.FileOffset == 9990 && rec.FirstFailure.Request.SourceSize == 11 &&
              rec.FirstFailure.Request.CancellationTag == 0x77,
          "the record has the request's offset, size and tag");
    check(std::wcscmp(rec.FirstFailure.Request.Filename, L"GAME:\\data\\b.bin") == 0,
          "the record names the file as the title opened it");
    check(std::strcmp(rec.FirstFailure.Request.Name, "past-the-end") == 0, "the record has the request's name");
    check(WaitForSingleObject(error_event, 0) == WAIT_TIMEOUT, "RetrieveErrorRecord resets the event");
    q->RetrieveErrorRecord(&rec);
    check(rec.FailureCount == 0 && rec.FirstFailure.CommandType == DSTORAGE_COMMAND_TYPE_NONE,
          "a second RetrieveErrorRecord has nothing");

    // Status and fence entries that fail.
    q->EnqueueStatus(s, 1000);
    q->EnqueueStatus(nullptr, 0);
    q->EnqueueSignal(nullptr, 1);
    q->EnqueueStatus(s, 31);
    q->Submit();
    check(wait_complete(s, 31), "the entries after the failed notifications complete");
    check_hr(s->GetHResult(31), S_OK, "notification failures do not fail the next status entry");
    check_hr(s->GetHResult(1000), E_DSTORAGE_INDEX_BOUND, "GetHResult past the array");
    check(s->IsComplete(1000), "IsComplete past the array is not pending");
    q->RetrieveErrorRecord(&rec);
    check(rec.FailureCount == 3 && rec.FirstFailure.CommandType == DSTORAGE_COMMAND_TYPE_STATUS &&
              rec.FirstFailure.HResult == E_DSTORAGE_INDEX_BOUND && rec.FirstFailure.Status.Index == 1000 &&
              rec.FirstFailure.Status.StatusArray == s,
          "an index past the array is recorded as a STATUS failure");

    VirtualFree(reserved, 0, MEM_RELEASE);
    CloseHandle(error_event);
    s->Release();
    q->Release();
    b->Release();
    big->Release();
}

void test_fence_order_and_events(Env& env) {
    IDStorageFileX* b = open(env, L"GAME:\\data\\b.bin");
    IDStorageQueueX2* q = make_queue(env, 0x200);
    TestFence fence;
    constexpr int kReads = 100;
    std::vector<std::vector<uint8_t>> outs(kReads, std::vector<uint8_t>(100));
    std::atomic<bool> in_place{false};
    fence.on_signal = [&](UINT64 v) {
        bool ok = v == 7;
        for (int i = 0; i < kReads; ++i) ok = ok && std::memcmp(outs[i].data(), env.b.data() + i * 100, 100) == 0;
        in_place.store(ok);
    };
    for (int i = 0; i < kReads; ++i) {
        DSTORAGE_REQUEST r = file_request(b, static_cast<uint64_t>(i) * 100, outs[i].data(), 100);
        q->EnqueueRequest(&r);
    }
    q->EnqueueSignal(&fence, 7);
    check(fence.refs.load() == 2, "an enqueued signal holds a fence reference");
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    q->EnqueueSetEvent(ev);
    q->Submit();
    check(WaitForSingleObject(ev, 10000) == WAIT_OBJECT_0, "EnqueueSetEvent sets the event");
    check(fence.GetCompletedValue() == 7, "EnqueueSignal signals the fence with its value");
    check(in_place.load(), "the fence is signalled after every read before it is in place");
    check(wait_until([&] { return fence.refs.load() == 1; }), "the fence reference is released after the signal");

    // A failed request before a fence: the fence still signals, and the status after the fence is clean.
    IDStorageStatusArrayX* s = make_status(env, 2);
    std::vector<uint8_t> tmp(8);
    DSTORAGE_REQUEST bad = file_request(b, 20000, tmp.data(), 8);
    q->EnqueueRequest(&bad);
    q->EnqueueSignal(&fence, 8);
    q->EnqueueStatus(s, 0);
    q->Submit();
    check(wait_complete(s, 0), "the status after a fence completes");
    check_hr(s->GetHResult(0), S_OK, "a failure before a fence is not reported by a status after it");
    check(fence.GetCompletedValue() == 8, "a fence after a failed read still signals");
    CloseHandle(ev);
    s->Release();
    q->Release();
    b->Release();
}

void test_cancel(Env& env) {
    IDStorageFileX* b = open(env, L"GAME:\\data\\b.bin");
    IDStorageQueueX2* q = make_queue(env, 0x80, false);
    IDStorageStatusArrayX* s = make_status(env, 1);
    std::vector<std::vector<uint8_t>> outs(6, std::vector<uint8_t>(16, 0));
    const uint64_t tags[] = {0x11, 0x12, 0x21, 0x22, 0x31};
    for (int i = 0; i < 5; ++i) {
        DSTORAGE_REQUEST r = file_request(b, 0, outs[i].data(), 16, tags[i]);
        q->EnqueueRequest(&r);
    }
    q->CancelRequestsWithTag(0xF0, 0x10);  // 0x11 and 0x12
    q->CancelRequestsWithTag(0xFF, 0x22);  // 0x22
    DSTORAGE_REQUEST later = file_request(b, 0, outs[5].data(), 16, 0x11);  // enqueued after: not cancelled
    q->EnqueueRequest(&later);
    q->EnqueueStatus(s, 0);
    q->Submit();
    check(wait_complete(s, 0), "cancelled requests complete");
    check_hr(s->GetHResult(0), S_OK, "a cancelled request is a success");
    const std::vector<uint8_t> zeros(16, 0);
    check(outs[0] == zeros && outs[1] == zeros && outs[3] == zeros, "cancelled requests read nothing");
    check(std::memcmp(outs[2].data(), env.b.data(), 16) == 0 && std::memcmp(outs[4].data(), env.b.data(), 16) == 0,
          "requests the mask did not match are read");
    check(std::memcmp(outs[5].data(), env.b.data(), 16) == 0, "a request enqueued after the cancel is read");
    s->Release();
    q->Release();
    b->Release();
}

void test_auto_submit_and_full_queue(Env& env) {
    IDStorageFileX* b = open(env, L"GAME:\\data\\b.bin");
    IDStorageQueueX2* q = make_queue(env, 0x80, true);
    std::vector<std::vector<uint8_t>> outs(64, std::vector<uint8_t>(8, 0));
    for (int i = 0; i < 63; ++i) {
        DSTORAGE_REQUEST r = file_request(b, i, outs[i].data(), 8);
        q->EnqueueRequest(&r);
    }
    DSTORAGE_QUEUE_INFO info{};
    q->Query(&info);
    check(info.RequestCountUntilAutoSubmit == 1, "one entry before the automatic submit");
    Sleep(20);
    check(outs[0] == std::vector<uint8_t>(8, 0), "below half the capacity nothing is submitted");
    DSTORAGE_REQUEST r = file_request(b, 63, outs[63].data(), 8);
    q->EnqueueRequest(&r);  // the 64th: half of 0x80
    check(wait_until([&] { return std::memcmp(outs[63].data(), env.b.data() + 63, 8) == 0; }),
          "reaching half the capacity submits without Submit");
    bool all = true;
    for (int i = 0; i < 64; ++i) all = all && std::memcmp(outs[i].data(), env.b.data() + i, 8) == 0;
    check(all, "every auto-submitted read has its bytes");
    q->Release();

    // Auto-submit disabled and full: an enqueue waits until another thread submits.
    q = make_queue(env, 0x80, false);
    IDStorageStatusArrayX* s = make_status(env, 1);
    constexpr int kMany = 400;
    std::vector<uint8_t> sink(kMany * 4);
    std::atomic<bool> done{false};
    std::thread producer([&] {
        for (int i = 0; i < kMany; ++i) {
            DSTORAGE_REQUEST rr = file_request(b, i, sink.data() + i * 4, 4);
            q->EnqueueRequest(&rr);
        }
        q->EnqueueStatus(s, 0);
        done.store(true);
    });
    check(wait_until([&] {
              DSTORAGE_QUEUE_INFO i{};
              q->Query(&i);
              return i.EmptySlotCount == 0;
          }),
          "the queue fills to capacity - 1");
    Sleep(20);
    check(!done.load(), "an enqueue into a full queue waits");
    while (!done.load()) {
        q->Submit();
        Sleep(1);
    }
    producer.join();
    q->Submit();
    check(wait_complete(s, 0), "the producer's reads complete");
    bool same = true;
    for (int i = 0; i < kMany; ++i) same = same && std::memcmp(sink.data() + i * 4, env.b.data() + i, 4) == 0;
    check(same, "every read of the full-queue run has its bytes");
    s->Release();
    q->Release();
    b->Release();
}

void test_memory_queue(Env& env) {
    IDStorageQueueX2* q = make_queue(env, 0x80, true, DSTORAGE_REQUEST_SOURCE_MEMORY, DSTORAGE_PRIORITY_REALTIME);
    IDStorageStatusArrayX* s = make_status(env, 6);
    std::vector<uint8_t> buf = pattern(8192, 9);
    std::vector<uint8_t> want = buf;
    std::memmove(want.data() + 1024, want.data(), 4096);
    DSTORAGE_REQUEST r{};
    r.Options.SourceType = DSTORAGE_REQUEST_SOURCE_MEMORY;
    r.Source = buf.data();
    r.Destination = buf.data() + 1024;
    r.SourceSize = r.DestinationSize = 4096;
    q->EnqueueRequest(&r);
    q->EnqueueStatus(s, 0);
    void* reserved = VirtualAlloc(nullptr, 1 << 16, MEM_RESERVE, PAGE_READWRITE);
    DSTORAGE_REQUEST bad = r;
    bad.Destination = reserved;
    q->EnqueueRequest(&bad);
    q->EnqueueStatus(s, 1);
    DSTORAGE_REQUEST file_on_memory{};  // a file request on a memory queue
    file_on_memory.Destination = buf.data();
    file_on_memory.SourceSize = file_on_memory.DestinationSize = 4;
    q->EnqueueRequest(&file_on_memory);
    q->EnqueueStatus(s, 2);
    DSTORAGE_REQUEST huge = r;
    huge.SourceSize = huge.DestinationSize = (1u << 29) + 1;  // more than 1 GiB together
    q->EnqueueRequest(&huge);
    q->EnqueueStatus(s, 3);
    // Buffers over 32 MiB need 64 KiB or 2 MiB pages (DSTORAGE_REQUEST). Reserved memory: a copy that is not refused
    // faults and reports an access violation instead.
    constexpr uint32_t k32MiB = 32u << 20;
    void* wide = VirtualAlloc(nullptr, 2 * (k32MiB + 4096), MEM_RESERVE, PAGE_READWRITE);
    check(wide != nullptr, "reserve 64 MiB for the over-limit copy");
    DSTORAGE_REQUEST over = r;
    over.Source = wide;
    over.Destination = static_cast<uint8_t*>(wide) + k32MiB + 4096;
    over.SourceSize = over.DestinationSize = k32MiB + 1;
    q->EnqueueRequest(&over);
    q->EnqueueStatus(s, 4);
    std::vector<uint8_t> at_limit_source = pattern(k32MiB, 10), at_limit(k32MiB, 0);
    DSTORAGE_REQUEST limit = r;
    limit.Source = at_limit_source.data();
    limit.Destination = at_limit.data();
    limit.SourceSize = limit.DestinationSize = k32MiB;
    q->EnqueueRequest(&limit);
    q->EnqueueStatus(s, 5);
    q->Submit();
    check(wait_complete(s, 5), "memory requests complete");
    check_hr(s->GetHResult(4), E_DSTORAGE_REQUEST_TOO_LARGE, "an unpaged buffer over 32 MiB is refused untouched");
    check_hr(s->GetHResult(5), S_OK, "a 32 MiB unpaged copy");
    check(at_limit == at_limit_source, "the 32 MiB copy moved every byte");
    VirtualFree(wide, 0, MEM_RELEASE);
    check_hr(s->GetHResult(0), S_OK, "a memory-to-memory copy");
    check(buf == want, "an overlapping memory copy moves like memmove");
    check_hr(s->GetHResult(1), E_DSTORAGE_ACCESS_VIOLATION, "a memory copy to memory the process cannot write");
    check_hr(s->GetHResult(2), E_DSTORAGE_INVALID_SOURCE_TYPE, "a file request on a memory queue");
    check_hr(s->GetHResult(3), E_DSTORAGE_REQUEST_TOO_LARGE, "source plus destination over 1 GiB");
    VirtualFree(reserved, 0, MEM_RELEASE);
    s->Release();
    q->Release();
}

void test_close_and_release(Env& env) {
    IDStorageFileX* big = open(env, L"GAME:\\data\\big.bin");
    // Close with requests in flight: it returns after they all ran.
    IDStorageQueueX2* q = make_queue(env, 0x100, true);
    constexpr int kReads = 300;
    constexpr uint32_t kChunk = 8192;
    std::vector<uint8_t> out(kReads * kChunk);
    for (int i = 0; i < kReads; ++i) {
        DSTORAGE_REQUEST r = file_request(big, static_cast<uint64_t>(i) * kChunk, out.data() + i * kChunk, kChunk);
        q->EnqueueRequest(&r);
    }
    q->Close();
    check(std::memcmp(out.data(), env.big.data(), out.size()) == 0, "Close returns after every enqueued read ran");
    IDStorageStatusArrayX* s = make_status(env, 1);
    std::vector<uint8_t> tmp(8, 0);
    DSTORAGE_REQUEST r = file_request(big, 0, tmp.data(), 8);
    q->EnqueueRequest(&r);
    q->EnqueueStatus(s, 0);
    q->Submit();
    q->Close();  // a second Close is harmless
    Sleep(20);
    check(tmp == std::vector<uint8_t>(8, 0), "a request enqueued after Close is not run");
    DSTORAGE_ERROR_RECORD rec{};
    q->RetrieveErrorRecord(&rec);
    check(rec.FailureCount == 2 && rec.FirstFailure.HResult == E_DSTORAGE_QUEUE_CLOSED,
          "enqueues after Close are recorded");
    check(q->Release() == 0, "a closed queue is freed by its last Release");

    // The last Release with requests in flight finishes them first.
    q = make_queue(env, 0x100, true);
    std::fill(out.begin(), out.end(), uint8_t{0});
    for (int i = 0; i < kReads; ++i) {
        DSTORAGE_REQUEST rr = file_request(big, static_cast<uint64_t>(i) * kChunk, out.data() + i * kChunk, kChunk);
        q->EnqueueRequest(&rr);
    }
    q->Submit();
    big->Release();  // the requests hold the file
    check(q->Release() == 0, "the last Release of a busy queue");
    check(std::memcmp(out.data(), env.big.data(), out.size()) == 0, "the last Release ran the reads in flight");

    // IDStorageFileX::Close: submitted reads finish, a later one fails with E_DSTORAGE_FILE_NOT_OPEN.
    IDStorageFileX* b = open(env, L"GAME:\\data\\b.bin");
    q = make_queue(env, 0x80, false);
    IDStorageStatusArrayX* s2 = make_status(env, 3);
    std::vector<uint8_t> first(100, 0), second(100, 0);
    DSTORAGE_REQUEST r1 = file_request(b, 0, first.data(), 100);
    q->EnqueueRequest(&r1);
    q->EnqueueStatus(s2, 0);
    q->Submit();
    DSTORAGE_REQUEST r2 = file_request(b, 0, second.data(), 100);
    q->EnqueueRequest(&r2);  // not submitted yet
    b->Close();
    check(std::memcmp(first.data(), env.b.data(), 100) == 0,
          "IDStorageFileX::Close waits for the file's submitted reads");
    check(wait_complete(s2, 0) && s2->GetHResult(0) == S_OK, "the submitted read succeeded");
    check(b->GetHandle() == INVALID_HANDLE_VALUE, "GetHandle on a closed file");
    q->EnqueueStatus(s2, 1);
    DSTORAGE_REQUEST r3 = file_request(b, 0, second.data(), 0);
    q->EnqueueRequest(&r3);
    q->EnqueueStatus(s2, 2);
    q->Submit();
    check(wait_complete(s2, 2), "the reads after the close complete");
    check_hr(s2->GetHResult(1), E_DSTORAGE_FILE_NOT_OPEN, "a read of a closed file");
    check_hr(s2->GetHResult(2), E_DSTORAGE_FILE_NOT_OPEN, "a zero-size read of a closed file");
    check(second == std::vector<uint8_t>(100, 0), "a read of a closed file writes nothing");
    q->Release();
    b->Release();
    s->Release();
    s2->Release();
}

void test_threads(Env& env) {
    IDStorageFileX* big = open(env, L"GAME:\\data\\big.bin");
    IDStorageQueueX2* q = make_queue(env, 0x80, true);
    constexpr int kThreads = 4, kPer = 250;
    constexpr uint32_t kChunk = 512;
    std::vector<std::vector<uint8_t>> outs(kThreads, std::vector<uint8_t>(kPer * kChunk));
    std::vector<IDStorageStatusArrayX*> arrays;
    for (int t = 0; t < kThreads; ++t) arrays.push_back(make_status(env, 1));
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t)
        threads.emplace_back([&, t] {
            for (int i = 0; i < kPer; ++i) {
                const uint64_t offset = (static_cast<uint64_t>(t) * kPer + i) * kChunk % (env.big.size() - kChunk);
                DSTORAGE_REQUEST r = file_request(big, offset, outs[t].data() + i * kChunk, kChunk, t);
                q->EnqueueRequest(&r);
            }
            q->EnqueueStatus(arrays[t], 0);
            q->Submit();
        });
    for (auto& th : threads) th.join();
    bool ok = true;
    for (int t = 0; t < kThreads; ++t) {
        ok = ok && wait_complete(arrays[t], 0) && arrays[t]->GetHResult(0) == S_OK;
        for (int i = 0; i < kPer; ++i) {
            const uint64_t offset = (static_cast<uint64_t>(t) * kPer + i) * kChunk % (env.big.size() - kChunk);
            ok = ok && std::memcmp(outs[t].data() + i * kChunk, env.big.data() + offset, kChunk) == 0;
        }
    }
    check(ok, "four threads enqueueing at once all read their bytes");
    for (auto* a : arrays) a->Release();
    q->Release();
    big->Release();
}

void test_iid_policy_events_and_cancel(Env& env) {
    // Well-known system interfaces and made-up ids: QueryInterface refuses all of them.
    const GUID kRandom = {0x5eed0001, 0x2222, 0x4222, {0x82, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x01}};
    const GUID kIMarshal = {0x00000003, 0x0000, 0x0000, {0xc0, 0, 0, 0, 0, 0, 0, 0x46}};
    const GUID kIAgileObject = {0x94ea2b94, 0xe9cc, 0x49e0, {0xc0, 0xff, 0xee, 0x64, 0xca, 0x8f, 0x5b, 0x90}};
    const GUID kINoMarshal = {0xecc8691b, 0xc1db, 0x4dc0, {0x85, 0x5e, 0x65, 0xf6, 0xc5, 0x51, 0xaf, 0x49}};
    const GUID kIClientSecurity = {0x0000013d, 0x0000, 0x0000, {0xc0, 0, 0, 0, 0, 0, 0, 0x46}};
    const GUID kPcStatusArrayId = {0x82397587, 0x7cd5, 0x453b, {0xa0, 0x2e, 0x31, 0x37, 0x9b, 0xd6, 0x46, 0x56}};
    const GUID kPcQueueId = {0xcfdbd83f, 0x9e06, 0x4fda, {0x8e, 0xa5, 0x69, 0x04, 0x21, 0x37, 0xf4, 0x9b}};
    const GUID kQueueId2 = {0x0d5e0012, 0x1111, 0x4111, {0x81, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x12}};
    const GUID refused[] = {kRandom, kIMarshal, kIAgileObject, kINoMarshal, kIClientSecurity};

    IDStorageQueueX2* q = make_queue(env, 0x80, false);
    IDStorageFileX* b = open(env, L"GAME:\\data\\b.bin");
    IDStorageStatusArrayX* s = make_status(env, 8);
    IUnknown* objects[] = {env.factory, q, b, s};
    for (IUnknown* o : objects)
        for (const GUID& g : refused) {
            void* out = reinterpret_cast<void*>(1);
            check_hr(o->QueryInterface(g, &out), E_NOINTERFACE, "QueryInterface refuses an unknown or system id");
            check(out == nullptr, "a refused QueryInterface clears ppv");
        }
    void* out = nullptr;
    check_hr(q->QueryInterface(kFileId, &out), E_NOINTERFACE, "a queue refuses a file id");
    check_hr(b->QueryInterface(kQueueId, &out), E_NOINTERFACE, "a file refuses a queue id");
    check_hr(s->QueryInterface(kFactoryId, &out), E_NOINTERFACE, "a status array refuses a factory id");
    check_hr(q->QueryInterface(__uuidof(IUnknown), &out), S_OK, "QueryInterface(IUnknown)");
    if (out) static_cast<IUnknown*>(out)->Release();

    // Creating calls learn a new id but refuse system ones.
    auto get = reinterpret_cast<DStorageGetFactoryFn>(rt::shims::find("dstorage_xs.dll", "DStorageGetFactory"));
    check_hr(get(kIMarshal, &out), E_NOINTERFACE, "DStorageGetFactory refuses IMarshal");
    check_hr(get(kIAgileObject, &out), E_NOINTERFACE, "DStorageGetFactory refuses IAgileObject");
    DSTORAGE_QUEUE_DESC1 d{};
    d.Capacity = 0x80;
    check_hr(env.factory->CreateQueue1(&d, kINoMarshal, &out), E_NOINTERFACE, "CreateQueue1 refuses INoMarshal");
    check_hr(env.factory->OpenFile(L"GAME:\\data\\b.bin", kIClientSecurity, &out), E_NOINTERFACE,
             "OpenFile refuses IClientSecurity");
    check_hr(env.factory->CreateQueue1(&d, kPcQueueId, &out), E_NOINTERFACE, "CreateQueue1 refuses the PC queue id");
    check_hr(env.factory->CreateQueue1(&d, kPcStatusArrayId, &out), E_NOINTERFACE,
             "the status array id is not a queue id");
    check_hr(env.factory->CreateQueue1(&d, kQueueId2, &out), S_OK, "a creating call learns a new queue id");
    if (out) static_cast<IUnknown*>(out)->Release();
    check_hr(q->QueryInterface(kQueueId2, &out), S_OK, "QueryInterface accepts an id a queue was created with");
    if (out) static_cast<IUnknown*>(out)->Release();

    // The PC IDStorageStatusArray id has the console interface's shape and is accepted.
    check_hr(env.factory->CreateStatusArray(2, nullptr, kPcStatusArrayId, &out), S_OK,
             "CreateStatusArray accepts the PC IDStorageStatusArray id");
    if (out) static_cast<IUnknown*>(out)->Release();
    check_hr(s->QueryInterface(kPcStatusArrayId, &out), S_OK, "a status array answers the PC IDStorageStatusArray id");
    if (out) static_cast<IUnknown*>(out)->Release();

    // EnqueueSetEvent duplicates the handle: closing the title's handle, even with its value reused, changes nothing.
    HANDLE first = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    q->EnqueueSetEvent(first);
    CloseHandle(first);
    HANDLE second = CreateEventW(nullptr, TRUE, FALSE, nullptr);  // may get the same handle value
    q->EnqueueSetEvent(reinterpret_cast<HANDLE>(static_cast<uintptr_t>(0x7ffffff0)));  // not a handle
    q->EnqueueSetEvent(nullptr);
    q->EnqueueStatus(s, 0);
    q->Submit();
    check(wait_complete(s, 0), "the event entries complete");
    check(WaitForSingleObject(second, 0) == WAIT_TIMEOUT, "a closed event's reused handle value is not set");
    DSTORAGE_ERROR_RECORD rec{};
    q->RetrieveErrorRecord(&rec);
    check(rec.FailureCount == 2 && rec.FirstFailure.CommandType == kCommandTypeEvent &&
              FAILED(rec.FirstFailure.HResult),
          "an invalid and a null event handle are recorded as EVENT failures; the closed one is not");
    CloseHandle(second);

    // A cancelled request that also had an enqueue error is still a success, and not counted as a failure.
    std::vector<uint8_t> tmp(8, 0);
    DSTORAGE_REQUEST foreign = file_request(reinterpret_cast<IDStorageFileX*>(s), 0, tmp.data(), 8, 0x99);
    q->EnqueueRequest(&foreign);
    DSTORAGE_REQUEST null_file = file_request(nullptr, 0, tmp.data(), 8, 0x99);
    q->EnqueueRequest(&null_file);
    q->CancelRequestsWithTag(~0ull, 0x99);
    q->EnqueueStatus(s, 1);
    q->Submit();
    check(wait_complete(s, 1), "cancelled bad requests complete");
    check_hr(s->GetHResult(1), S_OK, "a cancelled request with an enqueue error is a success");
    q->RetrieveErrorRecord(&rec);
    check(rec.FailureCount == 0, "a cancelled request is not counted as a failure");

    s->Release();
    b->Release();
    q->Release();
}

// Holds the queue's worker inside a copy: the copy's source is a guard page, and this handler waits on the first
// touch until the test releases it. The guard is gone after that fault, so the copy then goes on.
struct GuardHold {
    uint8_t* page = nullptr;
    HANDLE entered = nullptr;
    HANDLE release = nullptr;
};
GuardHold* g_hold = nullptr;

LONG CALLBACK hold_on_guard(EXCEPTION_POINTERS* x) {
    const EXCEPTION_RECORD* e = x->ExceptionRecord;
    if (!g_hold || e->ExceptionCode != STATUS_GUARD_PAGE_VIOLATION || e->NumberParameters < 2)
        return EXCEPTION_CONTINUE_SEARCH;
    const auto* address = reinterpret_cast<const uint8_t*>(e->ExceptionInformation[1]);
    if (address < g_hold->page || address >= g_hold->page + 4096) return EXCEPTION_CONTINUE_SEARCH;
    SetEvent(g_hold->entered);
    WaitForSingleObject(g_hold->release, 10000);
    return EXCEPTION_CONTINUE_EXECUTION;
}

// DSTORAGE_PRIORITY_STATS: TotalRequestCount and the byte totals count submitted requests; PendingRequestCount counts
// requests whose processing has started and not completed. A memory queue (realtime, the only priority it may have):
// the counts are compared with the ones before.
void test_stats_timing(Env& env) {
    auto realtime = [&] {
        DSTORAGE_STATS st{};
        env.factory->GetStats(&st);
        return st.RealtimePriorityStats;
    };
    const DSTORAGE_PRIORITY_STATS before = realtime();
    IDStorageQueueX2* q = make_queue(env, 0x80, false, DSTORAGE_REQUEST_SOURCE_MEMORY, DSTORAGE_PRIORITY_REALTIME);
    if (!q) return;
    IDStorageStatusArrayX* s = make_status(env, 1);
    GuardHold hold;
    hold.page = static_cast<uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    hold.entered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    hold.release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!hold.page || !hold.entered || !hold.release) {
        check(false, "allocate the guard page and its events");
        return;
    }
    std::memset(hold.page, 0x5A, 4096);
    DWORD old = 0;
    VirtualProtect(hold.page, 4096, PAGE_READWRITE | PAGE_GUARD, &old);
    std::vector<uint8_t> out(4096, 0);
    DSTORAGE_REQUEST r{};
    r.Options.SourceType = DSTORAGE_REQUEST_SOURCE_MEMORY;
    r.Source = hold.page;
    r.Destination = out.data();
    r.SourceSize = r.DestinationSize = 4096;
    q->EnqueueRequest(&r);
    q->EnqueueStatus(s, 0);
    DSTORAGE_PRIORITY_STATS p = realtime();
    check(p.TotalRequestCount == before.TotalRequestCount &&
              p.TotalRequestSourceByteCount == before.TotalRequestSourceByteCount &&
              p.TotalRequestDestinationByteCount == before.TotalRequestDestinationByteCount,
          "an enqueued request is not counted before Submit");

    g_hold = &hold;
    void* handler = AddVectoredExceptionHandler(1, hold_on_guard);
    q->Submit();
    check(WaitForSingleObject(hold.entered, 10000) == WAIT_OBJECT_0, "the copy reached the guard page");
    p = realtime();
    check(p.TotalRequestCount == before.TotalRequestCount + 1 &&
              p.TotalRequestSourceByteCount == before.TotalRequestSourceByteCount + 4096 &&
              p.TotalRequestDestinationByteCount == before.TotalRequestDestinationByteCount + 4096,
          "a submitted request is counted, with its sizes");
    check(p.PendingRequestCount == before.PendingRequestCount + 1, "a running request is pending");
    SetEvent(hold.release);
    check(wait_complete(s, 0), "the held copy completes");
    check_hr(s->GetHResult(0), S_OK, "the held copy succeeds");
    check(out == std::vector<uint8_t>(4096, 0x5A), "the held copy moved every byte");
    check(realtime().PendingRequestCount == before.PendingRequestCount, "a completed request is no longer pending");
    RemoveVectoredExceptionHandler(handler);
    g_hold = nullptr;
    CloseHandle(hold.entered);
    CloseHandle(hold.release);
    VirtualFree(hold.page, 0, MEM_RELEASE);
    s->Release();
    q->Release();
}

void test_stats(Env& env) {
    DSTORAGE_STATS st{};
    env.factory->GetStats(&st);
    check(st.CurrentOpenFileCount == 0, "every file was released");
    check(st.NormalPriorityStats.CurrentQueueCount == 0 && st.RealtimePriorityStats.CurrentQueueCount == 0,
          "every queue was released");
    check(st.NormalPriorityStats.TotalRequestCount > 1000 && st.NormalPriorityStats.TotalFenceCount >= 2 &&
              st.RealtimePriorityStats.TotalRequestCount == 7,
          "requests and fences are counted per priority");
    check(st.CumulativeOpenFileCount >= 10 && st.MaxOpenFileCount >= 3, "files are counted");
    check(st.ErrorCount > 10, "errors are counted");
}


// ---- zlib requests ------------------------------------------------------------------------------------------------
//
// Synthetic streams only. The two Huffman streams were made with Python's zlib (3.14, zlib-ng 1.3.1 API) from the
// text zlib_text() builds, which in Python is
//   text = lambda n: b"".join(b"line %04d: a zlib request for the queue, value %d\n" % (i, (i * i) % 97)
//                             for i in range(n))
// kZlibDynamic = zlib.compress(text(64), 9): one final dynamic-Huffman block (BTYPE 2), 345 bytes for 3,191.
// kZlibFixed = zlib.compress(text(4), 1): one final fixed-Huffman block (BTYPE 1), 77 bytes for 196.
// Streams with stored blocks (BTYPE 0) are built here by stored_stream(); for a single block that is byte for byte
// what zlib.compress(data, 0) gives.
constexpr uint8_t kZlibDynamic[] = {
    0x78, 0xda, 0x95, 0xd4, 0x4d, 0x4e, 0x03, 0x31, 0x0c, 0x05, 0xe0, 0x3d, 0xa7, 0xc8, 0x01, 0x58, 0x4c, 0xfc,
    0x97, 0x84, 0xdb, 0x14, 0x69, 0x10, 0x95, 0x46, 0x20, 0x4a, 0xcb, 0x82, 0xd3, 0x33, 0x0b, 0xf4, 0xbc, 0x7d,
    0xee, 0x6e, 0xa4, 0x3e, 0x25, 0xce, 0x67, 0xfb, 0xb8, 0x7e, 0xec, 0x6d, 0x3b, 0x7f, 0x2f, 0xed, 0xd2, 0x7e,
    0x8f, 0xeb, 0x6b, 0xbb, 0xed, 0x5f, 0x8f, 0xfd, 0xfb, 0xde, 0xde, 0x3e, 0x6f, 0xed, 0xfe, 0xbe, 0xb7, 0xf3,
    0xeb, 0xb1, 0x3f, 0xb7, 0x9f, 0xcb, 0xf1, 0x38, 0xff, 0xf9, 0x74, 0xfc, 0x07, 0x3a, 0x17, 0xe8, 0x08, 0x08,
    0x17, 0x30, 0x04, 0x94, 0x0b, 0x2c, 0x04, 0x8c, 0xbc, 0x52, 0x20, 0xe1, 0x5c, 0x42, 0x1c, 0x89, 0xe0, 0x12,
    0x9a, 0x67, 0x0c, 0xb2, 0xee, 0xac, 0x63, 0x72, 0x89, 0xc8, 0xa7, 0x5a, 0x5c, 0x62, 0x42, 0xa3, 0x93, 0xde,
    0x8a, 0x00, 0xe9, 0x2d, 0xb8, 0x54, 0x67, 0xc1, 0x07, 0x12, 0xa4, 0xf8, 0x10, 0x24, 0x48, 0xf2, 0x0c, 0x90,
    0xe2, 0x9a, 0x2f, 0x45, 0x8a, 0x47, 0x9e, 0x41, 0x8a, 0x2f, 0x74, 0x55, 0x27, 0xc5, 0x35, 0x39, 0x48, 0xf1,
    0x81, 0x81, 0x15, 0x52, 0xbc, 0xa3, 0x0e, 0x21, 0xc9, 0x1d, 0xb7, 0x12, 0x92, 0x7c, 0x61, 0x3a, 0x84, 0x24,
    0x37, 0xb4, 0x95, 0x90, 0xe4, 0x0b, 0x82, 0x42, 0x9a, 0x5b, 0xd6, 0x41, 0x9a, 0xaf, 0xbc, 0x15, 0x69, 0xee,
    0xe9, 0x41, 0x9a, 0x4f, 0x04, 0x48, 0xf2, 0x40, 0x5b, 0x29, 0x49, 0x2e, 0x98, 0x40, 0x25, 0xc9, 0x27, 0x6e,
    0xa5, 0x24, 0xb9, 0xe3, 0xa9, 0x94, 0x24, 0x17, 0x34, 0xa2, 0x92, 0xe4, 0x13, 0x2b, 0x54, 0x49, 0xf2, 0x40,
    0x93, 0x28, 0xbb, 0xd8, 0xf3, 0x75, 0x49, 0xf2, 0x9e, 0x67, 0xb0, 0xe4, 0x18, 0x0f, 0x65, 0xcd, 0x91, 0x30,
    0xd2, 0xdc, 0x20, 0x68, 0xa4, 0xb9, 0xc2, 0xc3, 0x48, 0xf3, 0x9e, 0x67, 0x90, 0xe6, 0x59, 0x06, 0x3b, 0xe5,
    0x98, 0x59, 0x23, 0xc9, 0x27, 0x00, 0x8d, 0x24, 0x1f, 0x68, 0x2b, 0x23, 0xc9, 0x47, 0x9e, 0x41, 0x92, 0x8f,
    0xac, 0x63, 0x55, 0x13, 0xbe, 0x55, 0x6f, 0xe5, 0xbd, 0x5a, 0xb9, 0x4b, 0xf5, 0x75, 0x5d, 0xab, 0x82, 0x6e,
    0xc5, 0x26, 0x71, 0xaf, 0xf6, 0xa1, 0x47, 0xb5, 0xd7, 0x7d, 0x54, 0xe7, 0xc9, 0x67, 0x75, 0x66, 0x7d, 0x55,
    0xf7, 0x42, 0x6c, 0xd5, 0xdd, 0x13, 0xbd, 0xba, 0xdf, 0x42, 0xaa, 0x3b, 0x34, 0x94, 0xde, 0xd3, 0x7f, 0x97,
    0x8b, 0x11, 0xce,
};
constexpr uint8_t kZlibFixed[] = {
    0x78, 0x01, 0xcb, 0xc9, 0xcc, 0x4b, 0x55, 0x30, 0x30, 0x30, 0x30, 0xb0, 0x52, 0x48, 0x54, 0xa8, 0xca, 0xc9,
    0x4c, 0x52, 0x28, 0x4a, 0x2d, 0x2c, 0x4d, 0x2d, 0x2e, 0x51, 0x48, 0xcb, 0x2f, 0x52, 0x28, 0xc9, 0x48, 0x55,
    0x28, 0x2c, 0x4d, 0x2d, 0x4d, 0xd5, 0x51, 0x28, 0x4b, 0xcc, 0x29, 0x4d, 0x55, 0x30, 0xe0, 0x82, 0x69, 0x30,
    0x24, 0x4e, 0x83, 0x21, 0x5c, 0x83, 0x11, 0x71, 0x1a, 0x4c, 0xe0, 0x1a, 0x8c, 0x89, 0xd3, 0x60, 0xc9, 0x05,
    0x00, 0xb0, 0x6e, 0x40, 0x49,
};

std::vector<uint8_t> zlib_text(int lines) {
    std::string t;
    char line[96];
    for (int i = 0; i < lines; ++i) {
        std::snprintf(line, sizeof line, "line %04d: a zlib request for the queue, value %d\n", i, (i * i) % 97);
        t += line;
    }
    return {t.begin(), t.end()};
}

uint32_t adler32(const std::vector<uint8_t>& data) {
    uint32_t a = 1, b = 0;
    for (const uint8_t x : data) {
        a = (a + x) % 65521;
        b = (b + a) % 65521;
    }
    return (b << 16) | a;
}

// A zlib stream (header 78 01) whose first block stores `stored` (a final block when there is no tail), followed by
// the deflate blocks of the zlib stream `tail` (its header and Adler-32 dropped: a stored block ends on a byte
// boundary, so they follow as they are) and the Adler-32 of the whole result, `stored` then `tail_plain`.
std::vector<uint8_t> stored_stream(const std::vector<uint8_t>& stored, const uint8_t* tail = nullptr,
                                   size_t tail_size = 0, const std::vector<uint8_t>& tail_plain = {}) {
    const auto n = static_cast<uint16_t>(stored.size());
    std::vector<uint8_t> z = {0x78, 0x01, static_cast<uint8_t>(tail ? 0 : 1), static_cast<uint8_t>(n),
                              static_cast<uint8_t>(n >> 8), static_cast<uint8_t>(~n), static_cast<uint8_t>(~n >> 8)};
    z.insert(z.end(), stored.begin(), stored.end());
    if (tail) z.insert(z.end(), tail + 2, tail + tail_size - 4);
    std::vector<uint8_t> plain = stored;
    plain.insert(plain.end(), tail_plain.begin(), tail_plain.end());
    const uint32_t check = adler32(plain);
    for (int shift = 24; shift >= 0; shift -= 8) z.push_back(static_cast<uint8_t>(check >> shift));
    return z;
}

// ---- DestinationIsPhysicalPages -------------------------------------------------------------------------------------

// The xmem shim's exports, resolved through the registry as a title's imports are (shims/xmem.cpp has the shapes).
using AllocatePagesFn = BOOL(WINAPI*)(uint64_t attributes, uint64_t* page_count, uint64_t* page_array);
using FreePagesFn = BOOL(WINAPI*)(uint64_t page_count, const uint64_t* page_array);
using XMemVirtualAllocFn = void*(WINAPI*)(void* address, SIZE_T size, DWORD type, ULONGLONG attributes, DWORD protect);
using MapPagesFn = void*(WINAPI*)(void* address, uint64_t page_count, const uint64_t* page_array);

constexpr size_t kPage = 0x10000;
constexpr DWORD kReserve64kWindow = MEM_RESERVE | 0x20400000;  // reserve only, 64 KiB pages: a physical-page window

PULONG_PTR page_array(const uint64_t* pages) { return reinterpret_cast<PULONG_PTR>(const_cast<uint64_t*>(pages)); }

// A destination given as 64 KiB page numbers and an offset into the first one. Every result is read back through a
// window the pages are mapped into, so the check is on the memory a title sees, not on the shim's own bookkeeping.
void test_destination_pages(Env& env) {
    const auto allocate = reinterpret_cast<AllocatePagesFn>(rt::shims::find("xmem.dll", "XMemAllocatePhysicalPages"));
    const auto free_pages = reinterpret_cast<FreePagesFn>(rt::shims::find("xmem.dll", "XMemFreePhysicalPages"));
    const auto reserve = reinterpret_cast<XMemVirtualAllocFn>(rt::shims::find("xmem.dll", "XMemVirtualAlloc"));
    const auto map_pages = reinterpret_cast<MapPagesFn>(rt::shims::find("xmem.dll", "XMemMapPhysicalPages"));
    using FreeWindowFn = BOOL(WINAPI*)(void*, SIZE_T, DWORD);
    const auto free_window = reinterpret_cast<FreeWindowFn>(rt::shims::find("kernel32.dll", "VirtualFree"));
    check(allocate && free_pages && reserve && map_pages && free_window, "the xmem page exports are registered");
    if (!allocate || !free_pages || !reserve || !map_pages || !free_window) return;

    // Two allocations, deliberately read out of page order. Small allocations may share a backing chunk;
    // the separate 513-page request below necessarily spans multiple chunks in this page store.
    uint64_t a[4] = {}, b[2] = {};
    uint64_t count_a = 4, count_b = 2;
    check(allocate(0x280, &count_a, a) && count_a == 4, "allocate four pages");
    check(allocate(0x280, &count_b, b) && count_b == 2, "allocate two more pages");
    const uint64_t all[6] = {a[0], a[1], a[2], a[3], b[0], b[1]};
    auto* const window = static_cast<uint8_t*>(reserve(nullptr, 6 * kPage, kReserve64kWindow, 1, PAGE_READWRITE));
    check(window != nullptr, "reserve a six-page window");
    if (!window) return;
    // Only the first three are mapped while the requests run: a page needs no mapping to be a destination.
    check(map_pages(window, 3, all) == window, "map the first three pages before the requests");
    std::vector<uint8_t> want(6 * kPage, 0);  // what the window must hold, page i of `all` at i * 64 KiB
    auto in_window = [&](uint64_t page) {
        return static_cast<size_t>(std::find(std::begin(all), std::end(all), page) - std::begin(all)) * kPage;
    };

    IDStorageFileX* big = open(env, L"GAME:\\data\\big.bin");
    IDStorageQueueX2* q = make_queue(env, 0x80, false);
    IDStorageQueueX2* mq =
        make_queue(env, 0x80, false, DSTORAGE_REQUEST_SOURCE_MEMORY, DSTORAGE_PRIORITY_REALTIME);
    IDStorageStatusArrayX* s = make_status(env, 16);

    // 0: a file read that starts 0xF000 bytes into a[1] and runs through a[2], b[0] and into a[3].
    const uint64_t file_pages[4] = {a[1], a[2], b[0], a[3]};
    constexpr uint32_t kFileBytes = 0x24000;
    constexpr uint16_t kFileOffset = 0xF000;
    DSTORAGE_REQUEST file = file_request(big, 12345, nullptr, kFileBytes);
    file.Options.DestinationIsPhysicalPages = 1;
    file.DestinationPageArray = page_array(file_pages);
    file.DestinationPageOffset = kFileOffset;
    q->EnqueueRequest(&file);
    q->EnqueueStatus(s, 0);
    {
        size_t done = 0, at = kFileOffset;
        for (const uint64_t page : file_pages) {
            const size_t n = (std::min)(kPage - at, kFileBytes - done);
            std::memcpy(want.data() + in_window(page) + at, env.big.data() + 12345 + done, n);
            done += n;
            at = 0;
        }
    }

    // 1: a page number that is not a live page (the second entry). Nothing may be written, the first page included.
    const uint64_t with_dead_page[2] = {a[0], a[3] + 16 * 1000};
    DSTORAGE_REQUEST dead = file_request(big, 0, nullptr, static_cast<uint32_t>(kPage));
    dead.Options.DestinationIsPhysicalPages = 1;
    dead.DestinationPageArray = page_array(with_dead_page);
    dead.DestinationPageOffset = 0x8000;
    q->EnqueueRequest(&dead);
    q->EnqueueStatus(s, 1);

    // 2: page number 0, which the map calls read as "unmap": not a destination.
    const uint64_t zero_page[1] = {0};
    DSTORAGE_REQUEST zero = file_request(big, 0, nullptr, 16);
    zero.Options.DestinationIsPhysicalPages = 1;
    zero.DestinationPageArray = page_array(zero_page);
    q->EnqueueRequest(&zero);
    q->EnqueueStatus(s, 2);

    // 3: a page array the process cannot read.
    void* unreadable = VirtualAlloc(nullptr, kPage, MEM_RESERVE, PAGE_READWRITE);
    DSTORAGE_REQUEST bad_array = file_request(big, 0, nullptr, 16);
    bad_array.Options.DestinationIsPhysicalPages = 1;
    bad_array.DestinationPageArray = static_cast<PULONG_PTR>(unreadable);
    q->EnqueueRequest(&bad_array);
    q->EnqueueStatus(s, 3);

    // 4: a zero-size request succeeds whatever its page array holds, like a zero-size read to an address.
    DSTORAGE_REQUEST empty = file_request(big, 0, nullptr, 0);
    empty.Options.DestinationIsPhysicalPages = 1;
    empty.DestinationPageArray = page_array(zero_page);
    q->EnqueueRequest(&empty);
    q->EnqueueStatus(s, 4);
    q->Submit();

    // 5: zlib from memory into b[1], ending in a[0]: the inflated bytes, not the stream, reach the pages.
    const std::vector<uint8_t> plain = zlib_text(64);
    const uint64_t zlib_pages[2] = {b[1], a[0]};
    constexpr uint16_t kZlibOffset = 0xF800;
    DSTORAGE_REQUEST inflate{};
    inflate.Options.SourceType = DSTORAGE_REQUEST_SOURCE_MEMORY;
    inflate.Options.ZlibDecompress = 1;
    inflate.Options.DestinationIsPhysicalPages = 1;
    inflate.Source = kZlibDynamic;
    inflate.SourceSize = static_cast<uint32_t>(sizeof kZlibDynamic);
    inflate.DestinationPageArray = page_array(zlib_pages);
    inflate.DestinationPageOffset = kZlibOffset;
    inflate.DestinationSize = static_cast<uint32_t>(plain.size());
    mq->EnqueueRequest(&inflate);
    mq->EnqueueStatus(s, 5);
    check(kZlibOffset + plain.size() > kPage, "the inflated text crosses into the second page");
    std::memcpy(want.data() + in_window(b[1]) + kZlibOffset, plain.data(), kPage - kZlibOffset);
    std::memcpy(want.data() + in_window(a[0]), plain.data() + (kPage - kZlibOffset),
                plain.size() - (kPage - kZlibOffset));

    // 6: a memory copy into a[3], after what request 0 left at its start.
    const std::vector<uint8_t> tail = pattern(0x700, 31);
    const uint64_t tail_page[1] = {a[3]};
    DSTORAGE_REQUEST copy{};
    copy.Options.SourceType = DSTORAGE_REQUEST_SOURCE_MEMORY;
    copy.Options.DestinationIsPhysicalPages = 1;
    copy.Source = tail.data();
    copy.SourceSize = copy.DestinationSize = static_cast<uint32_t>(tail.size());
    copy.DestinationPageArray = page_array(tail_page);
    copy.DestinationPageOffset = 0xF900;  // ends exactly at the end of the page
    mq->EnqueueRequest(&copy);
    mq->EnqueueStatus(s, 6);
    std::memcpy(want.data() + in_window(a[3]) + 0xF900, tail.data(), tail.size());
    mq->Submit();

    check(wait_complete(s, 4) && wait_complete(s, 6), "physical-page requests complete");
    check_hr(s->GetHResult(0), S_OK, "a file read into four physical pages from two allocations");
    check_hr(s->GetHResult(1), E_DSTORAGE_ACCESS_VIOLATION, "a page number that is not a live page");
    check_hr(s->GetHResult(2), E_DSTORAGE_ACCESS_VIOLATION, "page number 0 as a destination");
    check_hr(s->GetHResult(3), E_DSTORAGE_ACCESS_VIOLATION, "an unreadable page array");
    check_hr(s->GetHResult(4), S_OK, "a zero-size physical-page request");
    check_hr(s->GetHResult(5), S_OK, "zlib from memory into two physical pages");
    check_hr(s->GetHResult(6), S_OK, "a memory copy that ends at the end of a page");

    // The three pages that were mapped all along already show their bytes; the others are mapped now.
    check(std::memcmp(window, want.data(), 3 * kPage) == 0, "pages mapped during the requests hold the data");
    check(map_pages(window + 3 * kPage, 3, all + 3) == window + 3 * kPage, "map the other three pages");
    check(std::memcmp(window, want.data(), want.size()) == 0,
          "all six pages hold exactly the requests' bytes, zero elsewhere");

    VirtualFree(unreadable, 0, MEM_RELEASE);
    check(map_pages(window, 6, nullptr) == window, "unmap the window");
    check(free_pages(4, a) && free_pages(2, b), "free the pages");
    // A freed page is no longer a destination.
    IDStorageStatusArrayX* after = make_status(env, 1);
    DSTORAGE_REQUEST stale = file_request(big, 0, nullptr, 16);
    stale.Options.DestinationIsPhysicalPages = 1;
    stale.DestinationPageArray = page_array(tail_page);
    q->EnqueueRequest(&stale);
    q->EnqueueStatus(after, 0);
    q->Submit();
    check(wait_complete(after, 0), "the request on a freed page completes");
    check_hr(after->GetHResult(0), E_DSTORAGE_ACCESS_VIOLATION, "a freed page as a destination");

    // Over 32 MiB: a FILE source needs no memory-buffer page proof; its destination names actual 64 KiB pages.
    constexpr uint32_t kOver32MiB = (32u << 20) + 1;
    constexpr uint64_t kBigPages = (kOver32MiB + kPage - 1) / kPage;
    std::vector<uint64_t> big_pages(kBigPages);
    uint64_t big_count = kBigPages;
    const bool have_big = allocate(0x280, &big_count, big_pages.data()) && big_count == kBigPages;
    check(have_big, "allocate the pages of the large request");
    if (have_big) {
        const std::vector<uint8_t> source = pattern(kOver32MiB, 32);
        write_file(env.game / L"data" / L"large-page-destination.bin", source);
        IDStorageFileX* large_file = open(env, L"GAME:\\data\\large-page-destination.bin");
        DSTORAGE_REQUEST over = file_request(large_file, 0, nullptr, kOver32MiB);
        over.Options.DestinationIsPhysicalPages = 1;
        over.DestinationPageArray = page_array(big_pages.data());
        IDStorageStatusArrayX* big_status = make_status(env, 1);
        q->EnqueueRequest(&over);
        q->EnqueueStatus(big_status, 0);
        q->Submit();
        check(wait_complete(big_status, 0, 60000), "the request over 32 MiB completes");
        check_hr(big_status->GetHResult(0), S_OK, "over 32 MiB into physical pages");
        auto* const wide =
            static_cast<uint8_t*>(reserve(nullptr, kBigPages * kPage, kReserve64kWindow, 1, PAGE_READWRITE));
        const bool mapped = wide != nullptr && map_pages(wide, kBigPages, big_pages.data()) == wide;
        check(mapped, "map the large range");
        if (mapped) {
            check(std::memcmp(wide, source.data(), kOver32MiB) == 0, "every byte of the large copy is in the pages");
            check(wide[kOver32MiB] == 0, "the byte after the large copy is untouched");
            map_pages(wide, kBigPages, nullptr);
        }
        if (wide) check(free_window(wide, 0, MEM_RELEASE) != FALSE, "release the complete large physical window");
        free_pages(kBigPages, big_pages.data());
        big_status->Release();
        large_file->Release();
    }

    check(free_window(window, 0, MEM_RELEASE) != FALSE, "release the complete six-page physical window");
    after->Release();
    s->Release();
    mq->Release();
    q->Release();
    big->Release();
}

void test_zlib(Env& env) {
    const std::vector<uint8_t> dynamic(std::begin(kZlibDynamic), std::end(kZlibDynamic));
    const std::vector<uint8_t> fixed(std::begin(kZlibFixed), std::end(kZlibFixed));
    const std::vector<uint8_t> dynamic_plain = zlib_text(64), fixed_plain = zlib_text(4);
    // The first block header is the low three bits after the two header bytes: BFINAL, then BTYPE (RFC 1951 3.2.3).
    check(dynamic.size() == 345 && (dynamic[2] & 7) == (1 | 2 << 1), "kZlibDynamic is one final dynamic block");
    check(fixed.size() == 77 && (fixed[2] & 7) == (1 | 1 << 1), "kZlibFixed is one final fixed-Huffman block");
    check(dynamic_plain.size() == 3191 && fixed_plain.size() == 196, "the synthetic text has the generator's sizes");

    const std::vector<uint8_t> stored_part = pattern(300, 21);
    const std::vector<uint8_t> mixed = stored_stream(stored_part, kZlibDynamic, sizeof kZlibDynamic, dynamic_plain);
    std::vector<uint8_t> mixed_plain = stored_part;
    mixed_plain.insert(mixed_plain.end(), dynamic_plain.begin(), dynamic_plain.end());
    check((mixed[2] & 7) == 0, "the mixed stream starts with a stored block that is not the last");
    const std::vector<uint8_t> stored_only = stored_stream(pattern(40, 22));  // larger than what it inflates to
    std::vector<uint8_t> bad_method = dynamic, bad_type = fixed, bad_check = dynamic, preset = dynamic;
    bad_method[0] ^= 1;          // CM 9: not deflate
    preset[1] = 0xbb;            // 78 BB: a valid check value with FDICT set (a preset dictionary)
    bad_type[2] |= 0x06;         // BTYPE 3, reserved
    bad_check.back() ^= 1;       // the Adler-32

    // One file, every stream at a 16-byte aligned offset and followed by 32 zero bytes.
    std::vector<uint8_t> file;
    auto put = [&](const std::vector<uint8_t>& z) {
        const uint64_t at = file.size();
        file.insert(file.end(), z.begin(), z.end());
        file.resize((file.size() + 32 + 15) & ~size_t{15}, 0);
        return at;
    };
    const uint64_t at_dynamic = put(dynamic), at_fixed = put(fixed), at_mixed = put(mixed),
                   at_stored = put(stored_only), at_method = put(bad_method), at_preset = put(preset),
                   at_type = put(bad_type), at_check = put(bad_check);
    write_file(env.game / L"data" / L"zlib.bin", file);

    struct Case {
        uint64_t offset;
        uint32_t source, destination;
        HRESULT want;
        const std::vector<uint8_t>* result;  // the expected bytes; null when the destination stays untouched
        const char* what;
        uint64_t options = 0;
        uint32_t intermediate = 0;
    };
    const auto z = static_cast<uint32_t>(dynamic.size());
    std::vector<Case> cases = {
        {at_dynamic, z, 3191, S_OK, &dynamic_plain, "a zlib file request: dynamic Huffman"},
        {at_fixed, 77, 196, S_OK, &fixed_plain, "a zlib file request: fixed Huffman"},
        {at_mixed, static_cast<uint32_t>(mixed.size()), 3491, S_OK, &mixed_plain,
         "a zlib file request: a stored block, then dynamic Huffman"},
        {at_dynamic, z + 16, 3191, S_OK, &dynamic_plain, "bytes after the Adler-32 are not read"},
        {at_stored, static_cast<uint32_t>(stored_only.size()), 40, E_DSTORAGE_INVALID_DESTINATION_SIZE, nullptr,
         "a zlib DestinationSize not larger than SourceSize"},
        {at_method, z, 3191, E_DSTORAGE_ZLIB_BAD_HEADER, nullptr, "a header that is not deflate"},
        {at_preset, z, 3191, E_DSTORAGE_ZLIB_BAD_HEADER, nullptr, "a header with a preset dictionary"},
        {at_type, 77, 196, E_DSTORAGE_ZLIB_BAD_DATA, nullptr, "a reserved block type"},
        {at_dynamic, z - 10, 3191, E_DSTORAGE_ZLIB_BAD_DATA, nullptr, "a truncated stream"},
        {at_check, z, 3191, E_DSTORAGE_ZLIB_PARITY_FAIL, nullptr, "an Adler-32 mismatch"},
        {at_dynamic, z, 3192, E_DSTORAGE_DECOMPRESSION_ERROR, nullptr, "a stream shorter than DestinationSize"},
        {at_dynamic, z, 3190, E_DSTORAGE_DECOMPRESSION_ERROR, nullptr, "a stream longer than DestinationSize"},
        {at_dynamic + 8, z, 3191, E_DSTORAGE_INVALID_FILE_OFFSET, nullptr, "a zlib offset not 16-byte aligned"},
        {file.size() - 16, 32, 64, E_DSTORAGE_END_OF_FILE, nullptr, "a zlib source past the end of the file"},
        {at_dynamic, z, 3191, E_DSTORAGE_INVALID_INTERMEDIATE_SIZE, nullptr, "zlib with an IntermediateSize", 0,
         16},
        {at_dynamic, z, 3191, E_NOTIMPL, nullptr, "zlib and BC2 BCPack remain unsupported",
         static_cast<uint64_t>(DSTORAGE_BCPACK_MODE_BC2) << options::kBcpackShift, 64},
        {at_dynamic, z, 3191, E_NOTIMPL, nullptr, "zlib and swizzle is not implemented yet",
         static_cast<uint64_t>(DSTORAGE_SWIZZLE_MODE_4K_4K_8K) << options::kSwizzleShift},
    };

    DSTORAGE_STATS before{};
    env.factory->GetStats(&before);
    IDStorageFileX* f = open(env, L"GAME:\\data\\zlib.bin");
    IDStorageQueueX2* q = make_queue(env, 0x80, false);
    IDStorageStatusArrayX* s = make_status(env, static_cast<UINT32>(cases.size()));
    constexpr size_t kSlack = 64;  // past DestinationSize: must stay untouched
    std::vector<std::vector<uint8_t>> out(cases.size());
    for (size_t i = 0; i < cases.size(); ++i) {
        const Case& c = cases[i];
        out[i].assign(c.destination + kSlack, 0xEE);
        DSTORAGE_REQUEST r = file_request(f, c.offset, out[i].data(), c.destination);
        r.SourceSize = c.source;
        r.IntermediateSize = c.intermediate;
        r.Options.AsUINT64 = options::kZlib | c.options;
        q->EnqueueRequest(&r);
        q->EnqueueStatus(s, static_cast<UINT32>(i));
    }
    q->Submit();
    check(wait_complete(s, static_cast<UINT32>(cases.size() - 1)), "the zlib file requests complete");
    for (size_t i = 0; i < cases.size(); ++i) {
        const Case& c = cases[i];
        check_hr(s->GetHResult(static_cast<UINT32>(i)), c.want, c.what);
        const std::vector<uint8_t> untouched(c.destination, 0xEE);
        const bool head = c.result ? std::equal(c.result->begin(), c.result->end(), out[i].begin())
                                   : std::equal(untouched.begin(), untouched.end(), out[i].begin());
        const bool tail = std::all_of(out[i].end() - kSlack, out[i].end(), [](uint8_t x) { return x == 0xEE; });
        check(head && tail, c.result ? "the inflated bytes are the synthetic text, and nothing past them"
                                     : "a failed zlib request leaves the destination untouched");
    }
    // Several adjacent file inflates may finish in different orders. Only the FIFO writer
    // may publish overlapping data, and the first failing request supplies the status.
    std::vector<uint8_t> overlap(dynamic_plain.size(), 0xee);
    auto append = [&](uint64_t offset, uint32_t bytes, size_t start, size_t length) {
        auto r = file_request(f, offset, overlap.data() + start, static_cast<uint32_t>(length));
        r.Options.AsUINT64 = options::kZlib;
        r.SourceSize = bytes;
        q->EnqueueRequest(&r);
    };
    append(at_dynamic, z, 0, dynamic_plain.size());
    append(at_fixed, static_cast<uint32_t>(fixed.size()), 17, fixed_plain.size());
    append(at_check, z, 0, dynamic_plain.size());
    append(at_method, z, 0, dynamic_plain.size());
    q->EnqueueStatus(s, 0);
    q->Close(); // implicitly submits and waits for every result and its notification
    auto expected_overlap = dynamic_plain;
    std::copy(fixed_plain.begin(), fixed_plain.end(), expected_overlap.begin() + 17);
    check(overlap == expected_overlap, "overlapping file inflates commit in order through Close");
    check_hr(s->GetHResult(0), E_DSTORAGE_ZLIB_PARITY_FAIL, "the first failing file inflate supplies the status");
    s->Release();
    q->Release();
    f->Release();

    // A memory source: the same stream, one inflated in place (the compressed bytes sit at the end of their own
    // destination, as a title may load them), a corrupt one, a source and a destination the process cannot touch.
    IDStorageQueueX2* mq = make_queue(env, 0x80, true, DSTORAGE_REQUEST_SOURCE_MEMORY, DSTORAGE_PRIORITY_REALTIME);
    IDStorageStatusArrayX* ms = make_status(env, 5);
    void* reserved = VirtualAlloc(nullptr, 1 << 16, MEM_RESERVE, PAGE_READWRITE);
    std::vector<uint8_t> m0(3191, 0xEE), in_place(3191, 0xEE), m2(3191, 0xEE), m3(3191, 0xEE);
    std::copy(dynamic.begin(), dynamic.end(), in_place.end() - dynamic.size());
    DSTORAGE_REQUEST r{};
    r.Options.AsUINT64 = options::kZlib | options::kSourceMemory;
    r.SourceSize = z;
    r.DestinationSize = 3191;
    r.Source = dynamic.data();
    r.Destination = m0.data();
    mq->EnqueueRequest(&r);
    mq->EnqueueStatus(ms, 0);
    r.Source = in_place.data() + in_place.size() - dynamic.size();
    r.Destination = in_place.data();
    mq->EnqueueRequest(&r);
    mq->EnqueueStatus(ms, 1);
    r.Source = bad_check.data();
    r.Destination = m2.data();
    mq->EnqueueRequest(&r);
    mq->EnqueueStatus(ms, 2);
    r.Source = reserved;
    r.Destination = m3.data();
    mq->EnqueueRequest(&r);
    mq->EnqueueStatus(ms, 3);
    r.Source = dynamic.data();
    r.Destination = reserved;
    mq->EnqueueRequest(&r);
    mq->EnqueueStatus(ms, 4);
    mq->Submit();
    check(wait_complete(ms, 4), "the zlib memory requests complete");
    check_hr(ms->GetHResult(0), S_OK, "a zlib memory request");
    check(m0 == dynamic_plain, "the zlib memory request inflates to the synthetic text");
    check_hr(ms->GetHResult(1), S_OK, "a zlib memory request whose source lies inside its destination");
    check(in_place == dynamic_plain, "inflating in place gives the synthetic text");
    check_hr(ms->GetHResult(2), E_DSTORAGE_ZLIB_PARITY_FAIL, "a zlib memory request with an Adler-32 mismatch");
    check(m2 == std::vector<uint8_t>(3191, 0xEE), "the failed zlib memory request leaves the destination untouched");
    check_hr(ms->GetHResult(3), E_DSTORAGE_ACCESS_VIOLATION, "a zlib source the process cannot read");
    check_hr(ms->GetHResult(4), E_DSTORAGE_ACCESS_VIOLATION, "a zlib destination the process cannot write");
    VirtualFree(reserved, 0, MEM_RELEASE);
    ms->Release();
    mq->Release();

    // Counted when the inflate starts: the 15 file requests (6 good, 9 bad streams) and 4 memory requests (all but the
    // unreadable source) whose checks passed and whose source was read. Failed: the 9 bad file streams and the memory
    // one with an Adler-32 mismatch; a destination the process cannot write is not a decompression error.
    DSTORAGE_STATS after{};
    env.factory->GetStats(&after);
    check(after.TotalDecompressionRequestCount - before.TotalDecompressionRequestCount == 19,
          "TotalDecompressionRequestCount counts the zlib requests that reached the inflate");
    check(after.DecompressionErrors - before.DecompressionErrors == 10, "DecompressionErrors counts the bad streams");
    check(after.PendingDecompressionRequestCount == 0, "no decompression is pending afterwards");
}

// Strict native BCPack through the actual queue, with synthetic encoder-built data only. These tests run after
// the older aggregate-stat checks so new successful decompression requests cannot change their baselines.
void test_bcpack(Env& env) {
    using namespace dstorage_bcpack_fixture;
    const std::vector<uint8_t> packed(std::begin(kBc1), std::end(kBc1));
    const std::vector<uint8_t> compressed(std::begin(kZlibBc1), std::end(kZlibBc1));
    const auto stored = stored_stream(packed);
    std::vector<uint8_t> expected(kDecodedBytes);
    for (size_t at = 0; at < expected.size(); at += sizeof(kBlock))
        std::memcpy(expected.data() + at, kBlock, sizeof(kBlock));
    check(packed.size() < expected.size(), "the 512-block synthetic BC1 stream is smaller than its decoded output");
    check(stored.size() > packed.size(), "stored zlib plus BCPack has IntermediateSize smaller than SourceSize");

    std::vector<uint8_t> file_bytes(16, 0x5b);
    const auto append = [&](const std::vector<uint8_t>& bytes) {
        while (file_bytes.size() % 16) file_bytes.push_back(0x5b);
        const uint64_t offset = file_bytes.size();
        file_bytes.insert(file_bytes.end(), bytes.begin(), bytes.end());
        return offset;
    };
    const uint64_t at_packed = append(packed), at_compressed = append(compressed), at_stored = append(stored);
    write_file(env.game / L"data" / L"bcpack-synthetic.bin", file_bytes);
    IDStorageFileX* file = open(env, L"GAME:\\data\\bcpack-synthetic.bin");
    IDStorageQueueX2* fq = make_queue(env, 0x80, false);
    IDStorageQueueX2* mq = make_queue(env, 0x80, false, DSTORAGE_REQUEST_SOURCE_MEMORY, DSTORAGE_PRIORITY_REALTIME);
    IDStorageStatusArrayX* status = make_status(env, 96);
    if (!file || !fq || !mq || !status) return;
    DSTORAGE_STATS before{};
    env.factory->GetStats(&before);
    UINT32 slot = 0, starts = 0, decode_errors = 0, file_requests = 0, memory_requests = 0;
    UINT64 file_source_bytes = 0, file_destination_bytes = 0, memory_source_bytes = 0, memory_destination_bytes = 0;
    const auto run = [&](IDStorageQueueX2* q, const DSTORAGE_REQUEST& r, HRESULT want, const char* label,
                         bool starts_decode = true, bool decode_error = false) {
        const UINT32 index = slot++;
        check(index < 96, "BCPack status capacity covers every case");
        if (index >= 96) return;
        q->EnqueueRequest(&r);
        q->EnqueueStatus(status, index);
        q->Submit();
        check(wait_complete(status, index), label);
        check_hr(status->GetHResult(index), want, label);
        starts += starts_decode;
        decode_errors += decode_error;
        if (q == fq) {
            ++file_requests; file_source_bytes += r.SourceSize; file_destination_bytes += r.DestinationSize;
        } else {
            ++memory_requests; memory_source_bytes += r.SourceSize; memory_destination_bytes += r.DestinationSize;
        }
        DSTORAGE_STATS now{};
        env.factory->GetStats(&now);
        check(now.PendingDecompressionRequestCount == before.PendingDecompressionRequestCount,
              "BCPack completion balances the pending decompression count on every path");
    };
    const auto memory = [&](const void* source, size_t n, void* destination, bool zlib = false) {
        DSTORAGE_REQUEST r{};
        r.Options.SourceType = DSTORAGE_REQUEST_SOURCE_MEMORY;
        r.Options.BcpackMode = DSTORAGE_BCPACK_MODE_BC1;
        r.Options.ZlibDecompress = zlib;
        r.Source = source; r.SourceSize = static_cast<uint32_t>(n);
        r.Destination = destination; r.DestinationSize = kDecodedBytes;
        r.IntermediateSize = zlib ? static_cast<uint32_t>(packed.size()) : 0;
        return r;
    };
    const auto from_file = [&](uint64_t offset, size_t n, void* destination, bool zlib = false) {
        auto r = file_request(file, offset, destination, kDecodedBytes);
        r.SourceSize = static_cast<uint32_t>(n);
        r.Options.BcpackMode = DSTORAGE_BCPACK_MODE_BC1;
        r.Options.ZlibDecompress = zlib;
        r.IntermediateSize = zlib ? static_cast<uint32_t>(packed.size()) : 0;
        return r;
    };
    std::vector<uint8_t> output(kDecodedBytes, 0xee);
    for (const auto& r : {from_file(at_packed, packed.size(), output.data()),
                          from_file(at_compressed, compressed.size(), output.data(), true),
                          from_file(at_stored, stored.size(), output.data(), true)}) {
        std::fill(output.begin(), output.end(), uint8_t{0xee});
        run(fq, r, S_OK, "file BCPack or zlib-plus-BCPack request succeeds");
        check(output == expected, "file BCPack returns all 512 expected BC1 blocks");
    }
    for (const auto& r : {memory(packed.data(), packed.size(), output.data()),
                          memory(compressed.data(), compressed.size(), output.data(), true),
                          memory(stored.data(), stored.size(), output.data(), true)}) {
        std::fill(output.begin(), output.end(), uint8_t{0xee});
        run(mq, r, S_OK, "memory BCPack or zlib-plus-BCPack request succeeds");
        check(output == expected, "memory BCPack returns all 512 expected BC1 blocks");
    }
    for (bool zlib : {false, true}) {
        const auto& source = zlib ? compressed : packed;
        std::vector<uint8_t> in_place(kDecodedBytes, 0xee);
        std::memcpy(in_place.data() + 128, source.data(), source.size());
        auto r = memory(in_place.data() + 128, source.size(), in_place.data(), zlib);
        run(mq, r, S_OK, "overlapping BCPack source and destination succeed through staging");
        check(in_place == expected, "in-place BCPack has every expected byte");
    }
    const auto reject = [&](DSTORAGE_REQUEST r, HRESULT want, const char* label, bool started, bool decode_error) {
        std::vector<uint8_t> destination((std::max)(r.DestinationSize, kDecodedBytes), 0xe9);
        r.Destination = destination.data();
        run(r.Options.SourceType == DSTORAGE_REQUEST_SOURCE_MEMORY ? mq : fq, r, want, label, started, decode_error);
        check(destination == std::vector<uint8_t>(destination.size(), 0xe9),
              "a refused BCPack request preserves the entire destination");
    };
    auto r = memory(packed.data(), 1, nullptr);
    reject(r, E_DSTORAGE_DECOMPRESSION_ERROR, "truncated BCPack entropy is a decompression error", true, true);
    r = memory(kProvisional, sizeof(kProvisional), nullptr);
    reject(r, E_NOTIMPL, "unconfirmed BC1 history is refused without approximate output", true, true);
    r = memory(kUnknownBc4, sizeof(kUnknownBc4), nullptr); r.Options.BcpackMode = DSTORAGE_BCPACK_MODE_BC4;
    reject(r, E_NOTIMPL, "unknown BC4 history is refused without approximate output", true, true);
    r = memory(packed.data(), packed.size(), nullptr); r.Options.BcpackMode = DSTORAGE_BCPACK_MODE_BC2;
    reject(r, E_NOTIMPL, "BC2 remains unsupported", false, false);
    r = memory(packed.data(), packed.size(), nullptr); r.Options.SwizzleMode = DSTORAGE_SWIZZLE_MODE_4K_4K_8K;
    reject(r, E_NOTIMPL, "BCPack plus swizzle remains unsupported", false, false);
    r = from_file(at_packed + 1, packed.size(), nullptr);
    reject(r, E_DSTORAGE_INVALID_FILE_OFFSET, "BCPack file offsets must satisfy compressed-read alignment", false, false);
    r = memory(packed.data(), packed.size(), nullptr); r.IntermediateSize = 1;
    reject(r, E_DSTORAGE_INVALID_INTERMEDIATE_SIZE, "BCPack alone does not use IntermediateSize", false, false);
    r = memory(compressed.data(), compressed.size(), nullptr, true); r.IntermediateSize = 0;
    reject(r, E_DSTORAGE_INVALID_INTERMEDIATE_SIZE, "zlib-plus-BCPack requires an IntermediateSize", false, false);
    r = memory(compressed.data(), compressed.size(), nullptr, true); r.IntermediateSize = kMaxBcpackIntermediateSize + 1;
    reject(r, E_DSTORAGE_REQUEST_TOO_LARGE, "zlib-plus-BCPack honors the explicit intermediate staging limit", false, false);
    r = memory(compressed.data(), compressed.size(), nullptr, true); ++r.IntermediateSize;
    reject(r, E_DSTORAGE_DECOMPRESSION_ERROR, "zlib-plus-BCPack must produce exactly IntermediateSize", true, true);
    r = memory(packed.data(), packed.size(), nullptr); r.DestinationSize += sizeof(kBlock);
    reject(r, E_DSTORAGE_DECOMPRESSION_ERROR, "BCPack must produce exactly the declared destination size", true, true);
    void* unreadable = VirtualAlloc(nullptr, kPage, MEM_RESERVE, PAGE_READWRITE);
    check(unreadable != nullptr, "reserve an unreadable BCPack span");
    r = memory(unreadable, packed.size(), nullptr);
    reject(r, E_DSTORAGE_ACCESS_VIOLATION, "unreadable BCPack source is refused before decode", false, false);
    r = memory(packed.data(), packed.size(), unreadable);
    run(mq, r, E_DSTORAGE_ACCESS_VIOLATION, "unwritable BCPack destination is refused after decode", true, false);

    auto* mixed = static_cast<uint8_t*>(VirtualAlloc(nullptr, 8192, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    check(mixed != nullptr, "allocate mixed-protection BCPack destination");
    if (mixed) {
        std::memset(mixed, 0xd7, 8192);
        DWORD previous = 0;
        check(VirtualProtect(mixed + 4096, 4096, PAGE_NOACCESS, &previous) != FALSE, "protect the destination's second page");
        r = memory(packed.data(), packed.size(), mixed + 2048);
        run(mq, r, E_DSTORAGE_ACCESS_VIOLATION, "BCPack preflights the whole destination before writing its prefix", true, false);
        check(VirtualProtect(mixed + 4096, 4096, PAGE_READWRITE, &previous) != FALSE, "restore destination protection");
        check(std::all_of(mixed, mixed + 8192, [](uint8_t byte) { return byte == 0xd7; }),
              "mixed-protection failure preserves writable prefix and both-page canaries");
        VirtualFree(mixed, 0, MEM_RELEASE);
    }

#ifdef LEONIDA_DSTORAGE_TEST_HOOKS
    for (bool length_error : {false, true}) {
        for (const auto stage : {test::AllocationStage::source, test::AllocationStage::inflate, test::AllocationStage::bcpack}) {
            const bool inflate = stage == test::AllocationStage::inflate;
            r = memory(inflate ? compressed.data() : packed.data(), inflate ? compressed.size() : packed.size(), nullptr, inflate);
            test::fail_next_allocation(stage, length_error);
            const bool started = stage != test::AllocationStage::source;
            reject(r, E_OUTOFMEMORY, "BCPack allocation failure has an exact error and preserves destination", started, started);
        }
        // The pre-existing zlib-only path must balance the same guards when
        // staging or inflation allocation fails, not only the new combined path.
        for (const auto stage : {test::AllocationStage::source, test::AllocationStage::inflate}) {
            r = memory(compressed.data(), compressed.size(), nullptr, true);
            r.Options.BcpackMode = DSTORAGE_BCPACK_MODE_NONE;
            r.IntermediateSize = 0;
            r.DestinationSize = static_cast<uint32_t>(packed.size());
            test::fail_next_allocation(stage, length_error);
            const bool started = stage != test::AllocationStage::source;
            reject(r, E_OUTOFMEMORY, "zlib-only allocation failure balances stats and preserves destination", started, started);
        }
    }
#endif

    const auto allocate = reinterpret_cast<AllocatePagesFn>(rt::shims::find("xmem.dll", "XMemAllocatePhysicalPages"));
    const auto free_pages = reinterpret_cast<FreePagesFn>(rt::shims::find("xmem.dll", "XMemFreePhysicalPages"));
    const auto reserve = reinterpret_cast<XMemVirtualAllocFn>(rt::shims::find("xmem.dll", "XMemVirtualAlloc"));
    const auto map_pages = reinterpret_cast<MapPagesFn>(rt::shims::find("xmem.dll", "XMemMapPhysicalPages"));
    using FreeWindowFn = BOOL(WINAPI*)(void*, SIZE_T, DWORD);
    const auto free_window = reinterpret_cast<FreeWindowFn>(rt::shims::find("kernel32.dll", "VirtualFree"));
    uint64_t pages[2]{}, count = 2;
    const bool allocated = allocate && free_pages && reserve && map_pages && free_window && allocate(0x280, &count, pages) && count == 2;
    check(allocated, "allocate BCPack physical destination pages");
    auto* window = allocated ? static_cast<uint8_t*>(reserve(nullptr, 2 * kPage, kReserve64kWindow, 1, PAGE_READWRITE)) : nullptr;
    const bool mapped = window && map_pages(window, 2, pages) == window;
    check(mapped, "map the BCPack destination into a real CPU window");
    if (mapped) {
        constexpr uint16_t offset = 0xf800;
        const auto physical = [&](DSTORAGE_REQUEST request, const uint64_t* ids = nullptr) {
            request.Options.DestinationIsPhysicalPages = 1;
            request.DestinationPageArray = page_array(ids ? ids : pages);
            request.DestinationPageOffset = offset;
            return request;
        };
        for (auto request : {physical(from_file(at_packed, packed.size(), nullptr)),
                              physical(memory(packed.data(), packed.size(), nullptr)),
                              physical(memory(stored.data(), stored.size(), nullptr, true))}) {
            std::memset(window, 0xc6, 2 * kPage);
            run(request.Options.SourceType == DSTORAGE_REQUEST_SOURCE_MEMORY ? mq : fq, request, S_OK,
                "BCPack decoded bytes cross a physical-page boundary");
            std::vector<uint8_t> want(2 * kPage, 0xc6);
            std::memcpy(want.data() + offset, expected.data(), expected.size());
            check(std::memcmp(window, want.data(), want.size()) == 0, "physical BCPack writes decoded bytes and preserves surrounding pages");
        }
        const uint64_t bad_pages[2]{pages[0], 0};
        std::memset(window, 0xc6, 2 * kPage);
        r = physical(memory(packed.data(), packed.size(), nullptr), bad_pages);
        run(mq, r, E_DSTORAGE_ACCESS_VIOLATION, "BCPack dead second physical page preserves the first", true, false);
        check(std::all_of(window, window + 2 * kPage, [](uint8_t byte) { return byte == 0xc6; }), "bad BCPack page array causes no partial write");
        r = physical(memory(packed.data(), packed.size(), nullptr));
        r.DestinationPageArray = static_cast<PULONG_PTR>(unreadable);
        run(mq, r, E_DSTORAGE_ACCESS_VIOLATION, "BCPack unreadable physical-page array is refused", true, false);
        check(std::all_of(window, window + 2 * kPage, [](uint8_t byte) { return byte == 0xc6; }),
              "an unreadable BCPack page array preserves both destination pages");
#ifdef LEONIDA_DSTORAGE_TEST_HOOKS
        for (bool length_error : {false, true}) {
            r = physical(memory(packed.data(), packed.size(), nullptr));
            test::fail_next_allocation(test::AllocationStage::physical_page_array, length_error);
            run(mq, r, E_OUTOFMEMORY, "physical BCPack allocation failure balances pending statistics", true, false);
            check(std::all_of(window, window + 2 * kPage, [](uint8_t byte) { return byte == 0xc6; }), "physical BCPack allocation failure preserves every page");
        }
#endif
        map_pages(window, 2, nullptr);
    }
    if (window) check(free_window(window, 0, MEM_RELEASE) != FALSE, "release BCPack physical window");
    if (allocated) check(free_pages(2, pages) != FALSE, "free BCPack physical pages");
    if (unreadable) VirtualFree(unreadable, 0, MEM_RELEASE);
    DSTORAGE_STATS after{};
    env.factory->GetStats(&after);
    check(after.TotalDecompressionRequestCount - before.TotalDecompressionRequestCount == starts,
          "BCPack counts each decode request once, including zlib-plus-BCPack");
    check(after.DecompressionErrors - before.DecompressionErrors == decode_errors,
          "BCPack decompression errors exclude validation, source-read and final-write failures");
    check(after.PendingDecompressionRequestCount == before.PendingDecompressionRequestCount,
          "no BCPack decompression remains pending after success, malformed input and allocation failures");
    const auto compare_priority = [&](const DSTORAGE_PRIORITY_STATS& a, const DSTORAGE_PRIORITY_STATS& b,
                                      UINT32 requests, UINT64 source, UINT64 destination) {
        return a.TotalRequestCount - b.TotalRequestCount == requests &&
               a.TotalRequestSourceByteCount - b.TotalRequestSourceByteCount == source &&
               a.TotalRequestDestinationByteCount - b.TotalRequestDestinationByteCount == destination &&
               a.PendingRequestCount == b.PendingRequestCount;
    };
    check(compare_priority(after.NormalPriorityStats, before.NormalPriorityStats, file_requests, file_source_bytes, file_destination_bytes),
          "BCPack file queue accounts submitted request sizes and balances pending count");
    check(compare_priority(after.RealtimePriorityStats, before.RealtimePriorityStats, memory_requests, memory_source_bytes, memory_destination_bytes),
          "BCPack memory queue accounts submitted request sizes and balances pending count");
    status->Release(); mq->Release(); fq->Release(); file->Release();
}

// Physical sources are read from the persistent xmem backing, rather than by finding a current CPU mapping.
// All data below is synthetic. Requests and status markers go through the registered queue implementation.
void test_source_pages(Env& env) {
    using namespace dstorage_bcpack_fixture;
    const auto allocate = reinterpret_cast<AllocatePagesFn>(rt::shims::find("xmem.dll", "XMemAllocatePhysicalPages"));
    const auto free_pages = reinterpret_cast<FreePagesFn>(rt::shims::find("xmem.dll", "XMemFreePhysicalPages"));
    const auto reserve = reinterpret_cast<XMemVirtualAllocFn>(rt::shims::find("xmem.dll", "XMemVirtualAlloc"));
    const auto map_pages = reinterpret_cast<MapPagesFn>(rt::shims::find("xmem.dll", "XMemMapPhysicalPages"));
    using FreeWindowFn = BOOL(WINAPI*)(void*, SIZE_T, DWORD);
    const auto free_window = reinterpret_cast<FreeWindowFn>(rt::shims::find("kernel32.dll", "VirtualFree"));
    check(allocate && free_pages && reserve && map_pages && free_window, "physical source exports are registered");
    if (!allocate || !free_pages || !reserve || !map_pages || !free_window) return;

    uint64_t pages[6]{}, count = 6;
    const bool allocated = allocate(0x280, &count, pages) && count == 6;
    check(allocated, "allocate six physical source pages");
    if (!allocated) return;
    auto* window = static_cast<uint8_t*>(reserve(nullptr, 6 * kPage, kReserve64kWindow, 1, PAGE_READWRITE));
    auto* alias = static_cast<uint8_t*>(reserve(nullptr, 6 * kPage, kReserve64kWindow, 1, PAGE_READWRITE));
    const bool mapped = window && map_pages(window, 6, pages) == window;
    const bool alias_mapped = alias && map_pages(alias, 6, pages) == alias;
    check(mapped && alias_mapped, "map two CPU aliases of the same physical source pages");
    if (!mapped || !alias_mapped) {
        if (mapped) map_pages(window, 6, nullptr);
        if (alias_mapped) map_pages(alias, 6, nullptr);
        if (window) free_window(window, 0, MEM_RELEASE);
        if (alias) free_window(alias, 0, MEM_RELEASE);
        free_pages(6, pages);
        return;
    }
    IDStorageQueueX2* mq = make_queue(env, 0x80, false, DSTORAGE_REQUEST_SOURCE_MEMORY, DSTORAGE_PRIORITY_REALTIME);
    IDStorageStatusArrayX* status = make_status(env, 96);
    DSTORAGE_STATS before{};
    env.factory->GetStats(&before);
    UINT32 slot = 0, starts = 0, decode_errors = 0;
    UINT64 source_bytes = 0, destination_bytes = 0;
    const auto run = [&](const DSTORAGE_REQUEST& r, HRESULT want, const char* label,
                         bool starts_decode = false, bool decode_error = false) {
        const UINT32 index = slot++;
        check(index < 96, "physical source status capacity covers every case");
        if (index >= 96) return;
        mq->EnqueueRequest(&r);
        mq->EnqueueStatus(status, index);
        mq->Submit();
        check(wait_complete(status, index, 60000), label);
        check_hr(status->GetHResult(index), want, label);
        starts += starts_decode;
        decode_errors += decode_error;
        source_bytes += r.SourceSize;
        destination_bytes += r.DestinationSize;
        DSTORAGE_STATS now{};
        env.factory->GetStats(&now);
        check(now.PendingDecompressionRequestCount == before.PendingDecompressionRequestCount,
              "each physical source completion balances pending decompression");
    };
    const auto request = [&](const uint64_t* ids, uint16_t offset, uint32_t n, void* destination) {
        DSTORAGE_REQUEST r{};
        r.Options.SourceType = DSTORAGE_REQUEST_SOURCE_MEMORY;
        r.Options.SourceIsPhysicalPages = 1;
        r.SourcePageArray = page_array(ids);
        r.SourcePageOffset = offset;
        r.SourceSize = r.DestinationSize = n;
        r.Destination = destination;
        return r;
    };
    const auto initial = pattern(6 * kPage, 133);
    std::memcpy(window, initial.data(), initial.size());
    // UINT16's maximum offset needs three pages for 64 KiB + 2 bytes, in deliberately non-contiguous page order.
    const uint64_t reordered[3]{pages[2], pages[0], pages[1]};
    std::vector<uint8_t> output(kPage + 4, 0xe7), expected;
    expected.push_back(initial[3 * kPage - 1]);
    expected.insert(expected.end(), initial.begin(), initial.begin() + kPage + 1);
    auto r = request(reordered, 65535, static_cast<uint32_t>(expected.size()), output.data() + 1);
    run(r, S_OK, "physical source offset65535 spans three reordered pages");
    check(std::equal(expected.begin(), expected.end(), output.begin() + 1) && output.front() == 0xe7 && output.back() == 0xe7,
          "physical source page order, maximum offset and destination canaries are exact");
    alias[3 * kPage - 1] ^= 0x81;
    alias[17] ^= 0x44;
    expected[0] ^= 0x81;
    expected[18] ^= 0x44;
    run(r, S_OK, "physical reads observe writes through a second CPU alias");
    check(std::equal(expected.begin(), expected.end(), output.begin() + 1), "second CPU alias changes reach the source stage");
    check(map_pages(window, 6, nullptr) == window && map_pages(alias, 6, nullptr) == alias,
          "unmap both CPU aliases before a physical source request");
    std::fill(output.begin(), output.end(), uint8_t{0xe7});
    run(r, S_OK, "unmapped physical source pages remain readable");
    check(std::equal(expected.begin(), expected.end(), output.begin() + 1) && output.front() == 0xe7 && output.back() == 0xe7,
          "unmapped persistent backing preserves every byte and output canaries");
    check(map_pages(window, 6, pages) == window && map_pages(alias, 6, pages) == alias, "remap both physical source aliases");

    // An overlap across physical pages would destroy unread input if the worker wrote each source page as it read it.
    std::memcpy(window, initial.data(), initial.size());
    auto overlap_want = initial;
    std::memmove(overlap_want.data() + 0xf000, overlap_want.data() + 0xe000, 0x24000);
    r = request(pages, 0xe000, 0x24000, nullptr);
    r.Options.DestinationIsPhysicalPages = 1;
    r.DestinationPageArray = page_array(pages);
    r.DestinationPageOffset = 0xf000;
    run(r, S_OK, "overlapping physical source and destination are staged before any write");
    check(std::memcmp(window, overlap_want.data(), overlap_want.size()) == 0,
          "overlapping physical copy matches memmove with every surrounding byte preserved");
    std::memcpy(window, initial.data(), initial.size());
    overlap_want = initial;
    std::memmove(overlap_want.data() + 0xf000, overlap_want.data() + 0xe000, 0x24000);
    r = request(pages, 0xe000, 0x24000, alias + 0xf000);
    run(r, S_OK, "physical source can overlap an ordinary destination through another CPU alias");
    check(std::memcmp(window, overlap_want.data(), overlap_want.size()) == 0, "CPU-alias overlap also uses complete source staging");

    void* unreadable = VirtualAlloc(nullptr, kPage, MEM_RESERVE, PAGE_READWRITE);
    check(unreadable != nullptr, "reserve an unreadable physical source page-array span");
    std::vector<uint8_t> sentinel(8192, 0x9d);
    const auto refuse = [&](DSTORAGE_REQUEST bad, HRESULT want, const char* label, bool started = false, bool error = false) {
        std::fill(sentinel.begin(), sentinel.end(), uint8_t{0x9d});
        bad.Destination = sentinel.data();
        run(bad, want, label, started, error);
        check(std::all_of(sentinel.begin(), sentinel.end(), [](uint8_t b) { return b == 0x9d; }),
              "failed physical source request preserves the entire destination");
    };
    const uint64_t zero[1]{0}, invalid[1]{UINT64_MAX};
    refuse(request(zero, 0, 16, nullptr), E_DSTORAGE_ACCESS_VIOLATION, "page zero cannot be a physical source");
    refuse(request(invalid, 0, 16, nullptr), E_DSTORAGE_ACCESS_VIOLATION, "an invalid physical source page is refused");
    refuse(request(static_cast<const uint64_t*>(unreadable), 0, 16, nullptr), E_DSTORAGE_ACCESS_VIOLATION,
           "an unreadable physical source page array is refused");
    refuse(request(nullptr, 0, 16, nullptr), E_INVALIDARG, "a null physical source page array is invalid");
    uint64_t dead = 0, dead_count = 1;
    const bool have_dead = allocate(0x280, &dead_count, &dead) && dead_count == 1;
    check(have_dead, "allocate a separate page for a stale physical source ID");
    if (have_dead) {
        check(free_pages(1, &dead) != FALSE, "free the stale physical source page");
        const uint64_t partial[2]{pages[0], dead};
        refuse(request(partial, 65535, 2, nullptr), E_DSTORAGE_ACCESS_VIOLATION,
               "a freed second source page causes no partial destination write");
    }
    auto* split = static_cast<uint8_t*>(VirtualAlloc(nullptr, 8192, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    check(split != nullptr, "allocate a source array across two CPU protection pages");
    if (split) {
        std::memcpy(split + 4088, pages, 2 * sizeof(uint64_t));
        DWORD old = 0;
        check(VirtualProtect(split + 4096, 4096, PAGE_NOACCESS, &old) != FALSE, "protect the second source-array entry");
        refuse(request(reinterpret_cast<uint64_t*>(split + 4088), 65535, 2, nullptr), E_DSTORAGE_ACCESS_VIOLATION,
               "a partially unreadable source page array is rejected atomically");
        VirtualFree(split, 0, MEM_RELEASE);
    }
    for (const auto* ids : {zero, static_cast<const uint64_t*>(unreadable)})
        refuse(request(ids, 65535, 0, nullptr), S_OK, "zero-byte physical source does not dereference a nonnull page array");
    r = request(pages, 0, 16, nullptr); r.DestinationSize = 15;
    refuse(r, E_DSTORAGE_INVALID_DESTINATION_SIZE, "physical sources retain exact raw-copy size validation");

    // Place the entire encoded stream across an actual source-page boundary, then compare decoded bytes.
    const auto encoded = [&](const uint8_t* bytes, size_t n, uint32_t decoded, bool zlib, bool bcpack) {
        std::memset(window, 0xc3, 6 * kPage);
        std::memcpy(window + 65535, bytes, n);
        auto compressed = request(pages, 65535, static_cast<uint32_t>(n), sentinel.data());
        compressed.DestinationSize = decoded;
        compressed.Options.ZlibDecompress = zlib;
        compressed.Options.BcpackMode = bcpack ? DSTORAGE_BCPACK_MODE_BC1 : DSTORAGE_BCPACK_MODE_NONE;
        compressed.IntermediateSize = zlib && bcpack ? static_cast<uint32_t>(sizeof(kBc1)) : 0;
        return compressed;
    };
    const auto text = zlib_text(64);
    r = encoded(kZlibDynamic, sizeof(kZlibDynamic), static_cast<uint32_t>(text.size()), true, false);
    std::fill(sentinel.begin(), sentinel.end(), uint8_t{0x9d});
    run(r, S_OK, "zlib reads its compressed stream across physical source pages", true);
    check(std::equal(text.begin(), text.end(), sentinel.begin()) &&
              std::all_of(sentinel.begin() + text.size(), sentinel.end(), [](uint8_t b) { return b == 0x9d; }),
          "physical-source zlib produces exact decoded text and preserves its suffix");
    std::vector<uint8_t> blocks(kDecodedBytes);
    for (size_t at = 0; at < blocks.size(); at += sizeof(kBlock)) std::memcpy(blocks.data() + at, kBlock, sizeof(kBlock));
    for (bool zlib : {false, true}) {
        r = encoded(zlib ? kZlibBc1 : kBc1, zlib ? sizeof(kZlibBc1) : sizeof(kBc1), kDecodedBytes, zlib, true);
        std::fill(sentinel.begin(), sentinel.end(), uint8_t{0x9d});
        run(r, S_OK, "strict BC1 or zlib-plus-BC1 reads physical source pages", true);
        check(std::equal(blocks.begin(), blocks.end(), sentinel.begin()) &&
                  std::all_of(sentinel.begin() + blocks.size(), sentinel.end(), [](uint8_t b) { return b == 0x9d; }),
              "physical-source BCPack returns all 512 exact synthetic blocks and preserves suffix");
    }
    const std::vector<uint8_t> packed(std::begin(kBc1), std::end(kBc1));
    const auto stored = stored_stream(packed);
    r = encoded(stored.data(), stored.size(), kDecodedBytes, true, true);
    r.Options.DestinationIsPhysicalPages = 1;
    r.DestinationPageArray = page_array(pages + 3);
    r.DestinationPageOffset = 0xf800;
    auto physical_want = std::vector<uint8_t>(window, window + 6 * kPage);
    std::memcpy(physical_want.data() + 3 * kPage + 0xf800, blocks.data(), blocks.size());
    run(r, S_OK, "stored-zlib-plus-BC1 supports physical sources and destinations together", true);
    check(stored.size() > r.IntermediateSize && std::memcmp(window, physical_want.data(), physical_want.size()) == 0,
          "both-page compressed request allows IntermediateSize below SourceSize and preserves all surrounding bytes");
    r = encoded(kBc1, 1, kDecodedBytes, false, true);
    refuse(r, E_DSTORAGE_DECOMPRESSION_ERROR, "truncated physical-source entropy is an exact decompression error", true, true);
    r = encoded(kProvisional, sizeof(kProvisional), kDecodedBytes, false, true);
    refuse(r, E_NOTIMPL, "physical sources cannot bypass strict refusal of unconfirmed BC1 history", true, true);

#ifdef LEONIDA_DSTORAGE_TEST_HOOKS
    for (bool length_error : {false, true}) {
        for (const auto stage : {test::AllocationStage::source, test::AllocationStage::source_page_array,
                                test::AllocationStage::inflate, test::AllocationStage::bcpack}) {
            const bool inflate = stage == test::AllocationStage::inflate;
            r = encoded(inflate ? kZlibBc1 : kBc1, inflate ? sizeof(kZlibBc1) : sizeof(kBc1), kDecodedBytes, inflate, true);
            test::fail_next_allocation(stage, length_error);
            const bool started = stage == test::AllocationStage::inflate || stage == test::AllocationStage::bcpack;
            refuse(r, E_OUTOFMEMORY, "physical source allocation failures preserve destination and exact stats", started, started);
        }
        r = encoded(kBc1, sizeof(kBc1), kDecodedBytes, false, true);
        r.Options.DestinationIsPhysicalPages = 1;
        r.DestinationPageArray = page_array(pages + 3);
        r.DestinationPageOffset = 0xf800;
        const std::vector<uint8_t> before_oom(window, window + 6 * kPage);
        test::fail_next_allocation(test::AllocationStage::source_page_array, length_error);
        run(r, E_OUTOFMEMORY, "source-page array OOM leaves a physical destination untouched");
        check(std::memcmp(window, before_oom.data(), before_oom.size()) == 0, "source-page array OOM preserves all source/destination pages");
    }
#endif

    // Both buffers explicitly name 64 KiB pages, so the memory-request 1 GiB combined bound replaces both 32 MiB caps.
    constexpr uint32_t kOver32MiB = (32u << 20) + 1;
    constexpr uint64_t kBigPages = (kOver32MiB + kPage - 1) / kPage;
    std::vector<uint64_t> big_source(kBigPages), big_destination(kBigPages);
    uint64_t ns = kBigPages, nd = kBigPages;
    const bool have_source = allocate(0x280, &ns, big_source.data()) && ns == kBigPages;
    const bool have_destination = allocate(0x280, &nd, big_destination.data()) && nd == kBigPages;
    check(have_source && have_destination, "allocate source and destination for a physical copy over32MiB");
    auto* source_window = have_source ? static_cast<uint8_t*>(reserve(nullptr, kBigPages * kPage, kReserve64kWindow, 1, PAGE_READWRITE)) : nullptr;
    auto* destination_window = have_destination ? static_cast<uint8_t*>(reserve(nullptr, kBigPages * kPage, kReserve64kWindow, 1, PAGE_READWRITE)) : nullptr;
    const bool source_mapped = source_window && map_pages(source_window, kBigPages, big_source.data()) == source_window;
    const bool destination_mapped = destination_window && map_pages(destination_window, kBigPages, big_destination.data()) == destination_window;
    check(source_mapped && destination_mapped, "map both large physical ranges");
    if (source_mapped && destination_mapped) {
        const auto bytes = pattern(kOver32MiB, 141);
        std::memcpy(source_window, bytes.data(), bytes.size());
        std::memset(destination_window, 0xa6, kBigPages * kPage);
        check(map_pages(source_window, kBigPages, nullptr) == source_window, "unmap the large physical source before its queued read");
        r = request(big_source.data(), 0, kOver32MiB, nullptr);
        r.Options.DestinationIsPhysicalPages = 1;
        r.DestinationPageArray = page_array(big_destination.data());
        run(r, S_OK, "unmapped physical source and physical destination allow a copy over32MiB");
        check(std::memcmp(destination_window, bytes.data(), bytes.size()) == 0 &&
                  std::all_of(destination_window + bytes.size(), destination_window + kBigPages * kPage,
                              [](uint8_t b) { return b == 0xa6; }),
              "every byte of the large physical copy is exact and the full trailing page suffix is untouched");
        r.Options.SourceIsPhysicalPages = 0;
        r.Source = bytes.data();
        std::memset(destination_window, 0xa6, kBigPages * kPage);
        run(r, E_DSTORAGE_REQUEST_TOO_LARGE, "an ordinary source over32MiB is refused even with a physical destination");
        check(std::all_of(destination_window, destination_window + kBigPages * kPage, [](uint8_t b) { return b == 0xa6; }),
              "oversized ordinary-source rejection leaves the physical destination intact");
        r = request(big_source.data(), 0, kOver32MiB, sentinel.data());
        refuse(r, E_DSTORAGE_REQUEST_TOO_LARGE, "a physical source does not waive the ordinary destination32MiB cap");
    }
    // No huge allocation is needed to reject an oversized declared request before either page array is read.
    r = request(static_cast<const uint64_t*>(unreadable), 65535, (512u << 20) + 1, nullptr);
    r.Options.DestinationIsPhysicalPages = 1;
    r.DestinationPageArray = page_array(pages);
    const std::vector<uint8_t> before_limit(window, window + 6 * kPage);
    run(r, E_DSTORAGE_REQUEST_TOO_LARGE, "physical source plus destination cannot exceed the combined1GiB bound");
    check(std::memcmp(window, before_limit.data(), before_limit.size()) == 0, "combined-limit failure never reads bad arrays or writes destination");
    r.SourceSize = r.DestinationSize = UINT32_MAX;
    run(r, E_DSTORAGE_REQUEST_TOO_LARGE, "physical source/destination size sum is checked without32bit overflow");
    check(std::memcmp(window, before_limit.data(), before_limit.size()) == 0, "overflow-sized declaration preserves all destination pages");
    if (source_mapped) map_pages(source_window, kBigPages, nullptr);
    if (destination_mapped) map_pages(destination_window, kBigPages, nullptr);
    if (source_window) check(free_window(source_window, 0, MEM_RELEASE) != FALSE, "release the large physical source window");
    if (destination_window) check(free_window(destination_window, 0, MEM_RELEASE) != FALSE, "release the large physical destination window");
    if (have_source) check(free_pages(kBigPages, big_source.data()) != FALSE, "free large source pages");
    if (have_destination) check(free_pages(kBigPages, big_destination.data()) != FALSE, "free large destination pages");

    DSTORAGE_STATS after{};
    env.factory->GetStats(&after);
    check(after.TotalDecompressionRequestCount - before.TotalDecompressionRequestCount == starts &&
              after.DecompressionErrors - before.DecompressionErrors == decode_errors,
          "physical sources count decode starts/errors exactly, excluding validation and source staging failures");
    const auto& a = after.RealtimePriorityStats;
    const auto& b = before.RealtimePriorityStats;
    check(a.TotalRequestCount - b.TotalRequestCount == slot && a.TotalRequestSourceByteCount - b.TotalRequestSourceByteCount == source_bytes &&
              a.TotalRequestDestinationByteCount - b.TotalRequestDestinationByteCount == destination_bytes &&
              a.PendingRequestCount == b.PendingRequestCount && after.PendingDecompressionRequestCount == before.PendingDecompressionRequestCount,
          "physical source success/failure/OOM cases account all submitted sizes and balance every pending count");
    if (unreadable) VirtualFree(unreadable, 0, MEM_RELEASE);
    check(map_pages(window, 6, nullptr) == window && map_pages(alias, 6, nullptr) == alias, "unmap both final physical source aliases");
    check(free_window(window, 0, MEM_RELEASE) != FALSE && free_window(alias, 0, MEM_RELEASE) != FALSE, "release both physical source windows");
    check(free_pages(6, pages) != FALSE, "free physical source test pages");
    status->Release(); mq->Release();
}

}  // namespace

int main() {
    wchar_t temp[MAX_PATH];
    GetTempPathW(MAX_PATH, temp);
    Env env;
    env.root = fs::path(temp) / (L"leonida_dstorage_test_" + std::to_wstring(GetCurrentProcessId()));
    env.game = env.root / L"game";
    env.overlay = env.root / L"overlay";
    std::error_code ec;
    fs::remove_all(env.root, ec);
    env.a_package = pattern(4096, 1);
    env.a_overlay = pattern(5000, 2);  // another size and other bytes than the package's copy
    env.b = pattern(10000, 3);
    env.big = pattern(3u << 20, 4);
    write_file(env.game / L"data" / L"a.bin", env.a_package);
    write_file(env.overlay / L"data" / L"a.bin", env.a_overlay);
    write_file(env.game / L"data" / L"b.bin", env.b);
    write_file(env.game / L"data" / L"big.bin", env.big);
    rt::fs::configure(env.game, env.overlay);

    test_layout_and_options();
    test_registry(env);
    test_factory_checks(env);
    test_open_paths(env);
    test_reads_and_errors(env);
    test_fence_order_and_events(env);
    test_cancel(env);
    test_auto_submit_and_full_queue(env);
    test_memory_queue(env);
    test_close_and_release(env);
    test_threads(env);
    test_iid_policy_events_and_cancel(env);
    test_stats_timing(env);
    test_stats(env);
    test_zlib(env);  // after test_stats, whose request totals it would change
    test_destination_pages(env);
    test_bcpack(env);
    test_source_pages(env);

    fs::remove_all(env.root, ec);
    std::printf("dstorage: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
