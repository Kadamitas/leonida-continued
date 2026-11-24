#include <windows.h>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include "texture_core_bridge.h"
#include "leonida_texture_core.h"
#include "shims.h"

namespace {
int checks = 0, failures = 0;
void check(bool condition, const char* description) {
    ++checks;
    if (!condition) { ++failures; std::printf("FAIL: %s\n", description); }
}
template<class Function> Function get(const char* name) {
    auto* address = rt::shims::find("xg_xs.dll", name);
    check(address != nullptr, name);
    return reinterpret_cast<Function>(address);
}
void put32(uint8_t* data, size_t offset, uint32_t value) { std::memcpy(data+offset,&value,4); }
void put16(uint8_t* data, size_t offset, uint16_t value) { std::memcpy(data+offset,&value,2); }
void put64(uint8_t* data, size_t offset, uint64_t value) { std::memcpy(data+offset,&value,8); }
uint32_t read32(const uint8_t* data, size_t offset) { uint32_t value; std::memcpy(&value,data+offset,4); return value; }

std::array<uint8_t,56> descriptor(uint32_t width = 16, uint32_t height = 16, uint32_t layout = 1) {
    std::array<uint8_t,56> data{};
    put32(data.data(),0,3); put64(data.data(),0x10,width); put32(data.data(),0x18,height);
    put16(data.data(),0x1c,1); put16(data.data(),0x1e,1); put32(data.data(),0x20,28);
    put32(data.data(),0x24,1); put32(data.data(),0x2c,layout);
    return data;
}

template<class T> struct Guarded {
    uint64_t before = UINT64_C(0x1234567887654321);
    T value{};
    uint64_t after = UINT64_C(0x8765432112345678);
    void verify() const {
        check(before == UINT64_C(0x1234567887654321), "leading output guard");
        check(after == UINT64_C(0x8765432112345678), "trailing output guard");
    }
};

using Create = HRESULT(WINAPI*)(const void*,void**);
using Suggest = HRESULT(WINAPI*)(const void*,uintptr_t,uint32_t*);
using Release = uint32_t(WINAPI*)(void*);
using ResourceLayout = HRESULT(WINAPI*)(void*,uint8_t*);
using Size = uint64_t(WINAPI*)(void*);
using CopyInto = HRESULT(WINAPI*)(void*,uint8_t*,uint32_t,uint32_t,const uint8_t*,uint32_t,uint32_t);
using Tiling = HRESULT(WINAPI*)(void*,uint32_t*,rt::texture_core::PackedMipInfo*,rt::texture_core::TileShape*,uint32_t*,uint32_t,rt::texture_core::SubresourceTiling*);
const uintptr_t* table(void* object) { return *static_cast<const uintptr_t* const*>(object); }
void release(void* object) { check(reinterpret_cast<Release>(table(object)[1])(object) == 0, "native object release"); }

void native_layout_and_copy(Create create) {
    const auto d = descriptor();
    Guarded<void*> result;
    check(create(d.data(),&result.value) == S_OK && result.value, "measured two-argument constructor");
    result.verify();
    auto* object = result.value;
    Guarded<std::array<uint8_t,rt::texture_core::resource_layout_bytes>> layout;
    layout.value.fill(0xa5);
    check(reinterpret_cast<ResourceLayout>(table(object)[2])(object,layout.value.data()) == S_OK, "native resource-layout vtable");
    layout.verify();
    const auto bytes = reinterpret_cast<Size>(table(object)[3])(object);
    check(bytes >= 1024 && bytes < 0x100000, "bounded real texture allocation size");
    if (bytes < 1024 || bytes >= 0x100000) { release(object); return; }
    std::vector<uint8_t> resource(static_cast<size_t>(bytes)), source(1024), output(1024);
    for (size_t i = 0; i < source.size(); ++i) source[i] = static_cast<uint8_t>((i*29+7)%251);
    check(reinterpret_cast<CopyInto>(table(object)[8])(object,resource.data(),0,0,source.data(),64,1024) == S_OK, "real native copy-into");
    check(reinterpret_cast<CopyInto>(table(object)[9])(object,output.data(),0,0,resource.data(),64,1024) == S_OK, "real native copy-from");
    check(source == output, "CPU texture copy roundtrip preserves pixels");
    Guarded<uint32_t> total, count;
    Guarded<rt::texture_core::PackedMipInfo> packed;
    Guarded<rt::texture_core::TileShape> shape;
    Guarded<rt::texture_core::SubresourceTiling> record;
    total.value = 777; count.value = 1;
    std::memset(&packed.value,0xa5,sizeof(packed.value));
    std::memset(&shape.value,0xa5,sizeof(shape.value));
    std::memset(&record.value,0xa5,sizeof(record.value));
    const auto old_packed = packed.value;
    check(reinterpret_cast<Tiling>(table(object)[10])(object,&total.value,&packed.value,&shape.value,&count.value,0,&record.value) == E_NOTIMPL,
          "unsupported linear tiling reports failure");
    check(total.value == 777 && count.value == 1 && std::memcmp(&old_packed,&packed.value,sizeof(old_packed)) == 0,
          "unsupported tiling preserves outputs");
    total.verify(); count.verify(); packed.verify(); shape.verify(); record.verify();
    release(object);
}

void native_seven_argument_tiling(Create create) {
    const auto d = descriptor(112,80,0x119);
    void* object = nullptr;
    check(create(d.data(),&object) == S_OK && object, "64KiB layout object created");
    if (!object) return;
    Guarded<uint32_t> total, count;
    Guarded<rt::texture_core::PackedMipInfo> packed;
    Guarded<rt::texture_core::TileShape> shape;
    Guarded<rt::texture_core::SubresourceTiling> record;
    count.value = 1;
    check(reinterpret_cast<Tiling>(table(object)[10])(object,&total.value,&packed.value,&shape.value,&count.value,0,&record.value) == S_OK,
          "seven-argument tile metadata method");
    check(total.value == 1 && count.value == 1, "one independently calculated 64KiB tile");
    check(packed.value.standard_mips == 1 && packed.value.packed_mips == 0, "single standard mip");
    check(shape.value.width == 128 && shape.value.height == 128 && shape.value.depth == 1, "RGBA8 tile shape");
    check(record.value.width == 1 && record.value.height == 1 && record.value.depth == 1 && record.value.start_tile == 0,
          "standard mip tile coordinates");
    total.verify(); count.verify(); packed.verify(); shape.verify(); record.verify();
    release(object);
}

void suggestion_and_guest_guards(Create create, Suggest suggest) {
    auto d = descriptor();
    uint32_t expected = 0;
    check(lxg_suggest_swizzle(d.data(),d.size(),0,&expected) == S_OK, "backend reference suggestion");
    check(suggest(d.data(),0,reinterpret_cast<uint32_t*>(d.data()+0x2c)) == S_OK, "guest output aliases descriptor layout");
    check(read32(d.data(),0x2c) == expected && (expected & ~31u) == 0x100, "forwarded layout value");
    uint32_t sentinel = 0xdeadbeef;
    check(suggest(d.data(),1,&sentinel) == E_NOTIMPL && sentinel == 0xdeadbeef, "unsupported flags do not change output");
    void* object = reinterpret_cast<void*>(static_cast<uintptr_t>(1));
    check(create(nullptr,&object) == E_POINTER && !object, "null descriptor fails and clears output");
    check(create(reinterpret_cast<void*>(static_cast<uintptr_t>(1)),&object) == E_POINTER && !object, "unreadable descriptor guarded");
    check(create(d.data(),nullptr) == E_POINTER, "null publication pointer releases created object");
    check(suggest(nullptr,0,&sentinel) == E_POINTER && sentinel == 0xdeadbeef, "suggestion input fault preserves output");
    check(suggest(d.data(),0,nullptr) == E_POINTER, "suggestion output fault guarded");
    auto* pages = static_cast<uint8_t*>(VirtualAlloc(nullptr,8192,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
    check(pages != nullptr, "guarded guest pages allocated");
    if (!pages) return;
    DWORD old = 0;
    check(VirtualProtect(pages+4096,4096,PAGE_NOACCESS,&old) != 0, "guest trailing page protected");
    auto* boundary = pages+4096-d.size();
    std::memcpy(boundary,d.data(),d.size());
    check(create(boundary,&object) == S_OK && object, "constructor reads exactly56 bytes before protected page");
    if (object) release(object);
    check(create(boundary+1,&object) == E_POINTER && !object, "descriptor crossing guard page fails before Rust access");
    check(suggest(boundary+1,0,&sentinel) == E_POINTER && sentinel == 0xdeadbeef, "suggestion crossing guard page");
    std::memset(pages,0xcc,64);
    check(VirtualProtect(pages,4096,PAGE_READONLY,&old) != 0, "guest output made read-only");
    check(create(d.data(),reinterpret_cast<void**>(pages)) == E_POINTER, "failed object publication is guarded and released");
    check(suggest(d.data(),0,reinterpret_cast<uint32_t*>(pages)) == E_POINTER, "read-only suggestion output guarded");
    check(pages[0] == 0xcc && pages[7] == 0xcc, "failed publication leaves read-only output intact");
    VirtualFree(pages,0,MEM_RELEASE);
}
}  // namespace

int main() {
    check(lxg_abi_version() == 1, "texture backend ABI version");
    const auto create = get<Create>("XGCreateTextureComputer");
    const auto suggest = get<Suggest>("XGSuggestSwizzleMode");
    check(rt::shims::owns_dll("xg_xs.dll"), "XG module is owned by the registry");
    native_layout_and_copy(create);
    native_seven_argument_tiling(create);
    suggestion_and_guest_guards(create,suggest);
    std::printf("texture_core_bridge_test: %d checks, %d failures\n",checks,failures);
    return failures ? 1 : 0;
}
