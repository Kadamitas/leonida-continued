// Included inside xs_proxy_mock_test.cpp's anonymous namespace after its raw-call/COM fixtures.
// Child processes share only synthetic canaries/counters. No game, graphics driver or external service is used.
__declspec(noinline) void invoke_effect_probe(void* object, unsigned slot, EffectProbe* probe) {
    auto** table = *static_cast<void***>(object);
    probe->return_value = reinterpret_cast<SlotFn>(table[slot])(object, probe->args[1], probe->args[2], probe->args[3],
        0x5555, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    InterlockedIncrement(&probe->returned);  // retain this caller frame rather than tail-calling the proxy
}

int effect_child(unsigned which, HANDLE mapping) {
    auto* probe = static_cast<EffectProbe*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(EffectProbe)));
    if (!probe) return 2;
    const unsigned slot = which % 1000;
    Mock* mock = nullptr;
    void* device = make_device(mock, 9, true, true, true);
    if (!device) return 2;
    void* object = device;
    void* fence = nullptr;
    if (slot == 131 || slot == 132) {
        const IID aiid = __uuidof(ID3D12CommandAllocator), liid = __uuidof(ID3D12GraphicsCommandList);
        void* allocator = nullptr;
        const struct { UINT type, mask; } desc{D3D12_COMMAND_LIST_TYPE_DIRECT, 0};
        if (FAILED(hr_of(raw(device, 9, 0, word(&aiid), word(&allocator)))) || !allocator) return 2;
        object = nullptr;
        if (FAILED(hr_of(raw(device, 73, word(&desc), word(allocator), 0, word(&liid), word(&object)))) || !object) return 2;
    } else if (slot != 49) {
        object = make_queue(device);
        if (!object) return 2;
        if (slot == 14 || which >= 1000) { fence = make_fence(device); if (!fence) return 2; }
        if (which >= 1000 && FAILED(hr_of(raw(object, 16, word(fence), 1)))) return 2;
    }
    const auto base = reinterpret_cast<std::uint64_t>(GetModuleHandleW(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    std::uint64_t bounds[]{base, base + nt->OptionalHeader.SizeOfImage};
    rt::shims::add_module_lookup(bounds, [](void* ctx, std::uint64_t address, std::string& module, std::uint32_t& rva) {
        const auto* span = static_cast<const std::uint64_t*>(ctx);
        if (address < span[0] || address >= span[1]) return false;
        module = "synthetic-proxy-caller.exe"; rva = static_cast<std::uint32_t>(address - span[0]); return true;
    });
    wchar_t trace[MAX_PATH]{};
    if (!GetEnvironmentVariableW(L"LEONIDA_PROXY_TEST_TRACE", trace, MAX_PATH) || !rt::startup_trace::open(trace)) return 2;
    probe->args[0] = word(object);
    probe->args[1] = slot == 49 ? 0x11f : (which == 14 ? word(fence) : word(probe->output));
    probe->args[2] = slot == 49 ? 1 : 0x3333;
    probe->args[3] = 0x4444;
    probe->caller_begin_rva = word(&invoke_effect_probe) - base;
    probe->caller_end_rva = probe->caller_begin_rva + 0x200;
    g_effect_probe = probe;
    invoke_effect_probe(object, slot, probe);
    g_effect_probe = nullptr;
    rt::startup_trace::close();
    rt::shims::remove_module_lookup(bounds);
    UnmapViewOfFile(probe); CloseHandle(mapping);
    return 0;
}

int check_proxy_effect_policy() {
    struct Case { unsigned which; const wchar_t* policy; bool stops; unsigned backend_calls; } const cases[]{
        {131, L"stop", true, 0}, {132, L"stop", true, 0}, {31, L"stop", true, 0},
        {23, L"stop", true, 0}, {30, L"stop", true, 0}, {49, L"stop", true, 0},
        {1014, L"stop", true, 0}, {1015, L"stop", true, 0}, {1016, L"stop", true, 0}, {1017, L"stop", true, 0},
        {1032, L"stop", true, 0}, {14, L"stop", false, 1},
        {131, L"log", false, 0}, {30, L"log", false, 0}, {1032, L"log", false, 0}, {49, L"log", false, 1},
    };
    const auto environment = [](const wchar_t* name) {
        std::wstring value(32768, L'\0');
        const DWORD n = GetEnvironmentVariableW(name, value.data(), static_cast<DWORD>(value.size()));
        value.resize(n < value.size() ? n : 0); return value;
    };
    const auto old_policy = environment(L"LEONIDA_UNIMPLEMENTED"), old_trace = environment(L"LEONIDA_PROXY_TEST_TRACE");
    wchar_t exe[32768]{}, temp[MAX_PATH]{};
    EXPECT(GetModuleFileNameW(nullptr, exe, 32768) && GetTempPathW(MAX_PATH, temp));
    for (const auto& c : cases) {
        const unsigned slot = c.which % 1000;
        wchar_t trace[MAX_PATH]{}; EXPECT(GetTempFileNameW(temp, L"lps", 0, trace));
        EXPECT(DeleteFileW(trace));  // startup_trace deliberately creates new files exclusively
        SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
        HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, 0, sizeof(EffectProbe), nullptr);
        EXPECT(mapping);
        auto* probe = static_cast<EffectProbe*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(EffectProbe)));
        EXPECT(probe); std::memset(probe, 0, sizeof(*probe));
        for (auto& pattern : probe->output) pattern = 0xa5a5a5a5a5a5a5a5;
        SetEnvironmentVariableW(L"LEONIDA_UNIMPLEMENTED", c.policy);
        SetEnvironmentVariableW(L"LEONIDA_PROXY_TEST_TRACE", trace);
        std::wstring command = L"\"" + std::wstring(exe) + L"\" --proxy-effect-child " + std::to_wstring(c.which) + L" " +
            std::to_wstring(reinterpret_cast<std::uintptr_t>(mapping));
        STARTUPINFOW startup{}; startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        const BOOL created = CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &startup, &process);
        SetEnvironmentVariableW(L"LEONIDA_UNIMPLEMENTED", old_policy.empty() ? nullptr : old_policy.c_str());
        SetEnvironmentVariableW(L"LEONIDA_PROXY_TEST_TRACE", old_trace.empty() ? nullptr : old_trace.c_str());
        EXPECT(created);
        const DWORD waited = WaitForSingleObject(process.hProcess, 15000);
        if (waited != WAIT_OBJECT_0) { TerminateProcess(process.hProcess, 99); WaitForSingleObject(process.hProcess, 1000); }
        DWORD exit_code = UINT32_MAX; GetExitCodeProcess(process.hProcess, &exit_code);
        CloseHandle(process.hThread); CloseHandle(process.hProcess);
        std::ifstream file(std::filesystem::path(trace), std::ios::binary);
        const std::string events((std::istreambuf_iterator<char>(file)), {});
        file.close(); DeleteFileW(trace);
        if (exit_code != (c.stops ? 3u : 0u)) std::fprintf(stderr, "guard child slot%u exit%lu\n", slot, exit_code);
        EXPECT(waited == WAIT_OBJECT_0 && exit_code == (c.stops ? 3u : 0u));
        EXPECT(probe->returned == (c.stops ? 0 : 1) && probe->backend_calls == static_cast<LONG>(c.backend_calls));
        for (const auto pattern : probe->output) EXPECT(pattern == 0xa5a5a5a5a5a5a5a5);
        if (c.stops) {
            const char* kind = slot >= 131 ? "list" : slot == 49 ? "device" : "queue";
            EXPECT(events.find("\"kind\":\"unimplemented_proxy_call\"") != std::string::npos &&
                   events.find("\"object_kind\":\"" + std::string(kind) + "\"") != std::string::npos &&
                   events.find("\"slot\":" + std::to_string(slot)) != std::string::npos);
            const std::string prefix = "synthetic-proxy-caller.exe+0x";
            const auto at = events.find(prefix); EXPECT(at != std::string::npos);
            const auto caller = std::strtoull(events.c_str() + at + prefix.size(), nullptr, 16);
            EXPECT(caller >= probe->caller_begin_rva && caller < probe->caller_end_rva);
            const char* names[]{"rcx", "rdx", "r8", "r9"};
            for (unsigned i = 0; i < 4; ++i)
                EXPECT(events.find("\"" + std::string(names[i]) + "\":\"" + rt::startup_trace::hex(probe->args[i]) + "\"") != std::string::npos);
        } else {
            EXPECT(events.find("unimplemented_proxy_call") == std::string::npos);
            if (slot == 30 || c.which == 1032) EXPECT(hr_of(probe->return_value) == E_NOTIMPL);
        }
        UnmapViewOfFile(probe); CloseHandle(mapping);
    }
    return 0;
}
