// Opt-in read-only package source. Metadata and verified snapshots only; no backing handle or path escapes.
#pragma once
#include <windows.h>
#include <cstdint>
#include <memory>
#include <string_view>

namespace rt::partial_package {
struct File;
using FileRef = std::shared_ptr<const File>;

// Both LEONIDA_STREAM_PACKAGE and LEONIDA_STREAM_INDEX must name the pinned local package/index.
bool enabled();
FileRef find(std::wstring_view package_relative);
std::uint64_t size(const FileRef& file);

// Synchronous, explicit-offset read. Logical EOF succeeds with a short/zero count. Maximum request256MiB.
// All covering pieces are independently hashed and retained before any destination write. A missing piece
// waits at most10seconds, then fails ERROR_NOT_READY. Any failure leaves destination unchanged and count0.
bool read_at(const FileRef& file, std::uint64_t offset, void* destination, std::uint32_t length,
             std::uint32_t& read_bytes, DWORD& error);
}  // namespace rt::partial_package
