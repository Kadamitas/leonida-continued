#pragma once

#include "../src/shader_library.h"

#include <windows.h>

#include <cstdio>
#include <filesystem>
#include <span>
#include <string>

namespace rt::d3d12 {

// On an offline translator miss, preserve the supplied container and its binding/signature metadata.
// Different containers can hold the same native program, so keep their hashes distinct.
inline void capture_missing_graphics_shader(const char* stage, const std::string& program_hash,
                                            std::span<const std::uint8_t> bytecode) {
    char selected[65]{};
    if (GetEnvironmentVariableA("LEONIDA_CAPTURE_GRAPHICS_SHA256", selected, sizeof(selected)) != 64 ||
        program_hash != selected || bytecode.size() < 4 || bytecode.size() > 8 * 1024 * 1024) return;

    const bool container = std::memcmp(bytecode.data(), "DXBC", 4) == 0;
    if (!container && std::memcmp(bytecode.data(), "FSSM", 4) != 0) return;
    const DWORD folder_size = GetEnvironmentVariableW(L"LEONIDA_CAPTURE_GRAPHICS_DIR", nullptr, 0);
    if (!folder_size) return;
    std::wstring folder(folder_size, L'\0');
    if (GetEnvironmentVariableW(L"LEONIDA_CAPTURE_GRAPHICS_DIR", folder.data(), folder_size) != folder_size - 1) return;
    folder.resize(folder_size - 1);
    const std::filesystem::path directory(folder);
    if (!directory.is_absolute()) return;

    const auto path = directory / (std::string(stage) + "-" + leonida::sha256_hex(bytecode) +
                                   (container ? ".dxbc" : ".fssm"));
    if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) return;
    auto pending = path;
    pending += L"." + std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(GetCurrentThreadId()) + L".tmp";
    HANDLE file = CreateFileW(pending.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    const bool complete = WriteFile(file, bytecode.data(), static_cast<DWORD>(bytecode.size()), &written, nullptr)
                          && written == bytecode.size();
    const bool closed = CloseHandle(file) != 0;
    if (complete && closed && MoveFileW(pending.c_str(), path.c_str())) {
        std::fprintf(stderr, "[rt] d3d12: captured missing %s shader bytes=%zu program=%s\n", stage, bytecode.size(),
                     program_hash.c_str());
    } else {
        DeleteFileW(pending.c_str());
    }
}

} // namespace rt::d3d12
