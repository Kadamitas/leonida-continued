#include "partial_package.h"

#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <climits>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace rt::partial_package {
namespace {
constexpr std::uint64_t kTotal = 223880932870ull;
constexpr std::uint32_t kPiece = 16777216;
constexpr std::uint32_t kFiles = 108;
constexpr std::uint32_t kPieces = 13345;
constexpr std::uint32_t kMaximumRead = 256u * 1024 * 1024;
constexpr std::size_t kCacheBytes = 128u * 1024 * 1024;
constexpr char kInfoHash[] = "465d6a0eac381e68ca77115f44f19f0c968b9590";
// Hash-only commitment to make_stream_index.py's deterministic metadata serialization. Merely repeating the
// torrent info-hash in a header would not authenticate an altered piece-hash table.
constexpr char kIndexHash[] = "b8c3dc96914f3e0f4fcb307ffe96c51171a9f0faf8d748d4010e87da796e0275";

struct Handle {
    HANDLE value = INVALID_HANDLE_VALUE;
    explicit Handle(HANDLE h) : value(h) {}
    ~Handle() { if (value != INVALID_HANDLE_VALUE && value) ::CloseHandle(value); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};

bool digest_matches(const unsigned char* digest, std::size_t bytes, const char* expected) {
    constexpr char hex[] = "0123456789abcdef";
    for (std::size_t i = 0; i < bytes; ++i)
        if (hex[digest[i] >> 4] != expected[2*i] || hex[digest[i] & 15] != expected[2*i+1]) return false;
    return true;
}

bool hash_bytes(LPCWSTR algorithm, const std::vector<unsigned char>& input, unsigned char* digest, ULONG bytes) {
    struct Algorithm {
        BCRYPT_ALG_HANDLE value = nullptr;
        ~Algorithm() { if (value) BCryptCloseAlgorithmProvider(value, 0); }
    } alg;
    // BCrypt keeps this caller-owned object storage until BCryptDestroyHash. Declare it before the handle
    // guard so destruction closes the hash before freeing its backing bytes on every return path.
    std::vector<unsigned char> object;
    struct Hash {
        BCRYPT_HASH_HANDLE value = nullptr;
        ~Hash() { if (value) BCryptDestroyHash(value); }
    } hash;
    if (BCryptOpenAlgorithmProvider(&alg.value, algorithm, nullptr, 0) < 0) return false;
    ULONG object_size = 0, got = 0, hash_size = 0;
    if (BCryptGetProperty(alg.value, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&object_size),
                          sizeof(object_size), &got, 0) < 0 || got != sizeof(object_size) || object_size > (1u << 20))
        return false;
    if (BCryptGetProperty(alg.value, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&hash_size), sizeof(hash_size),
                          &got, 0) < 0 || got != sizeof(hash_size) || hash_size != bytes) return false;
    object.resize(object_size);
    if (BCryptCreateHash(alg.value, &hash.value, object.data(), object_size, nullptr, 0, 0) < 0) return false;
    if (input.size() > std::numeric_limits<ULONG>::max() ||
        BCryptHashData(hash.value, const_cast<PUCHAR>(input.data()), static_cast<ULONG>(input.size()), 0) < 0)
        return false;
    return BCryptFinishHash(hash.value, digest, bytes, 0) >= 0;
}

std::wstring environment(const wchar_t* name) {
    const DWORD needed = ::GetEnvironmentVariableW(name, nullptr, 0);
    if (!needed || needed > 32768) return {};
    std::wstring value(needed, L'\0');
    const DWORD length = ::GetEnvironmentVariableW(name, value.data(), needed);
    if (!length || length >= needed) return {};
    value.resize(length);
    return value;
}

bool same(std::wstring_view a, std::wstring_view b) {
    return a.size() <= INT_MAX && b.size() <= INT_MAX &&
           ::CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}

bool local_absolute(std::wstring_view path) {
    if (path.starts_with(L"\\\\?\\")) path.remove_prefix(4);
    return path.size() >= 3 && ((path[0] >= L'A' && path[0] <= L'Z') || (path[0] >= L'a' && path[0] <= L'z')) &&
           path[1] == L':' && (path[2] == L'\\' || path[2] == L'/');
}

std::wstring final_path(HANDLE handle) {
    const DWORD needed = ::GetFinalPathNameByHandleW(handle, nullptr, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (!needed || needed > 32768) return {};
    std::wstring path(needed, L'\0');
    const DWORD n = ::GetFinalPathNameByHandleW(handle, path.data(), needed, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (!n || n >= needed) return {};
    path.resize(n);
    return local_absolute(path) ? path : std::wstring{};
}

bool relative_path(std::wstring_view input, std::wstring& output) {
    if (input.empty() || input.size() > 4096) return false;
    output.assign(input);
    std::replace(output.begin(), output.end(), L'/', L'\\');
    std::size_t begin = 0;
    while (begin < output.size()) {
        const std::size_t end = output.find(L'\\', begin);
        const auto part = std::wstring_view(output).substr(begin, (end == std::wstring::npos ? output.size() : end) - begin);
        if (part.empty() || part == L"." || part == L".." || part.back() == L'.' || part.back() == L' ') return false;
        for (wchar_t c : part) if (c < 32 || c == L':' || c == L'<' || c == L'>' || c == L'"' || c == L'|' || c == L'?' || c == L'*') return false;
        const auto base = part.substr(0, part.find(L'.'));
        if (same(base, L"CON") || same(base, L"PRN") || same(base, L"AUX") || same(base, L"NUL")) return false;
        if (base.size() == 4 && base[3] >= L'1' && base[3] <= L'9' &&
            (same(base.substr(0,3), L"COM") || same(base.substr(0,3), L"LPT"))) return false;
        if (end == std::wstring::npos) return true;
        begin = end + 1;
    }
    return false;
}

struct Cursor {
    const std::vector<unsigned char>& bytes;
    std::size_t at = 0;
    const unsigned char* take(std::size_t n) {
        if (at > bytes.size() || n > bytes.size()-at) throw std::runtime_error("truncated metadata index");
        const auto* p = bytes.data()+at;
        at += n;
        return p;
    }
    template<class T> T integer() { T value; std::memcpy(&value, take(sizeof(value)), sizeof(value)); return value; }
};

bool read_exact(HANDLE handle, std::uint64_t offset, unsigned char* out, DWORD length) {
    LARGE_INTEGER position{};
    position.QuadPart = static_cast<LONGLONG>(offset);
    if (!::SetFilePointerEx(handle, position, nullptr, FILE_BEGIN)) return false;
    DWORD done = 0;
    while (done < length) {
        DWORD got = 0;
        if (!::ReadFile(handle, out+done, length-done, &got, nullptr)) return false;
        if (!got) { ::SetLastError(ERROR_HANDLE_EOF); return false; }
        done += got;
    }
    return true;
}

bool writable(void* destination, std::size_t length) {
    auto at = reinterpret_cast<std::uintptr_t>(destination);
    if (!at || length > std::numeric_limits<std::uintptr_t>::max()-at) return false;
    const auto end = at+length;
    while (at < end) {
        MEMORY_BASIC_INFORMATION info{};
        if (!::VirtualQuery(reinterpret_cast<void*>(at), &info, sizeof(info)) || info.State != MEM_COMMIT ||
            (info.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
        const DWORD access = info.Protect & 0xff;
        if (access != PAGE_READWRITE && access != PAGE_WRITECOPY && access != PAGE_EXECUTE_READWRITE && access != PAGE_EXECUTE_WRITECOPY) return false;
        const auto base = reinterpret_cast<std::uintptr_t>(info.BaseAddress);
        if (info.RegionSize > std::numeric_limits<std::uintptr_t>::max()-base || base+info.RegionSize <= at) return false;
        at = std::min(end, base+info.RegionSize);
    }
    return true;
}

struct State;
using PieceRef = std::shared_ptr<const std::vector<unsigned char>>;
}  // namespace

struct File {
    State* owner;
    std::wstring relative;
    std::uint64_t offset, length;
};

namespace {
struct State {
    bool valid = false;
    std::wstring root;
    std::vector<FileRef> files;
    std::vector<std::array<unsigned char,20>> hashes;
    struct Cached { PieceRef bytes; std::uint64_t age; };
    std::mutex mutex;
    std::map<std::uint32_t,Cached> cache;
    std::size_t cache_bytes = 0;
    std::uint64_t age = 0;

    State() {
        const auto package = environment(L"LEONIDA_STREAM_PACKAGE");
        const auto index = environment(L"LEONIDA_STREAM_INDEX");
        if (package.empty() && index.empty()) return;
        if (!local_absolute(package) || !local_absolute(index)) throw std::runtime_error("both stream paths must be local absolute paths");
        Handle directory(::CreateFileW(package.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                       nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
        if (directory.value == INVALID_HANDLE_VALUE) throw std::runtime_error("stream source directory unavailable");
        BY_HANDLE_FILE_INFORMATION directory_info{};
        if (!::GetFileInformationByHandle(directory.value, &directory_info) || !(directory_info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            throw std::runtime_error("stream source is not a directory");
        root = final_path(directory.value);
        if (root.empty()) throw std::runtime_error("cannot resolve local stream source directory");
        if (root.back() != L'\\') root += L'\\';
        Handle input(::CreateFileW(index.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        LARGE_INTEGER length{};
        if (input.value == INVALID_HANDLE_VALUE || !::GetFileSizeEx(input.value, &length) || length.QuadPart < 64 || length.QuadPart > (1 << 20))
            throw std::runtime_error("stream metadata index size/open invalid");
        std::vector<unsigned char> bytes(static_cast<std::size_t>(length.QuadPart));
        if (!read_exact(input.value, 0, bytes.data(), static_cast<DWORD>(bytes.size()))) throw std::runtime_error("stream index read failed");
        std::array<unsigned char,32> digest{};
        if (!hash_bytes(BCRYPT_SHA256_ALGORITHM, bytes, digest.data(), static_cast<ULONG>(digest.size())) ||
            !digest_matches(digest.data(), digest.size(), kIndexHash)) throw std::runtime_error("stream metadata index commitment mismatch");
        Cursor cursor{bytes};
        if (std::memcmp(cursor.take(8), "LPPIDX01", 8) || cursor.integer<std::uint32_t>() != 1 ||
            cursor.integer<std::uint32_t>() != kPiece || cursor.integer<std::uint64_t>() != kTotal ||
            cursor.integer<std::uint32_t>() != kFiles || cursor.integer<std::uint32_t>() != kPieces ||
            !digest_matches(cursor.take(20), 20, kInfoHash) || cursor.integer<std::uint32_t>() != 0 ||
            cursor.integer<std::uint64_t>() != bytes.size()-64) throw std::runtime_error("stream metadata header differs");
        std::uint64_t next = 0;
        for (std::uint32_t i = 0; i < kFiles; ++i) {
            const auto offset = cursor.integer<std::uint64_t>();
            const auto size = cursor.integer<std::uint64_t>();
            const auto count = cursor.integer<std::uint32_t>();
            if (offset != next || size > kTotal-next || !count || count > 4096) throw std::runtime_error("stream file table bounds differ");
            const auto* encoded = cursor.take(count);
            const int characters = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, reinterpret_cast<const char*>(encoded), static_cast<int>(count), nullptr, 0);
            if (characters <= 0) throw std::runtime_error("stream path is not UTF8");
            std::wstring decoded(static_cast<std::size_t>(characters), L'\0'), relative;
            if (::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, reinterpret_cast<const char*>(encoded), static_cast<int>(count), decoded.data(), characters) != characters ||
                !relative_path(decoded, relative)) throw std::runtime_error("stream path escaped package");
            for (const auto& f : files) if (same(f->relative, relative)) throw std::runtime_error("duplicate stream path");
            files.push_back(std::make_shared<const File>(File{this, std::move(relative), offset, size}));
            next += size;
        }
        if (next != kTotal) throw std::runtime_error("stream file table has a gap");
        hashes.resize(kPieces);
        for (auto& hash : hashes) std::memcpy(hash.data(), cursor.take(hash.size()), hash.size());
        if (cursor.at != bytes.size()) throw std::runtime_error("trailing stream metadata");
        valid = true;
        std::fprintf(stderr, "[rt] partial_package: pinned metadata enabled; reads require independently verified complete pieces\n");
    }

    PieceRef cached(std::uint32_t index) {
        std::lock_guard lock(mutex);
        const auto it = cache.find(index);
        if (it == cache.end()) return {};
        it->second.age = ++age;
        return it->second.bytes;
    }

    void retain(std::uint32_t index, PieceRef bytes) {
        std::lock_guard lock(mutex);
        if (cache.contains(index)) return;
        while (cache_bytes+bytes->size() > kCacheBytes && !cache.empty()) {
            auto oldest = std::min_element(cache.begin(), cache.end(), [](const auto& a, const auto& b) { return a.second.age < b.second.age; });
            cache_bytes -= oldest->second.bytes->size();
            cache.erase(oldest);
        }
        const auto bytes_size = bytes->size();
        cache.emplace(index, Cached{std::move(bytes), ++age});
        cache_bytes += bytes_size;
    }

    PieceRef load(std::uint32_t index, DWORD& error) {
        if (auto found = cached(index)) return found;
        if (index >= hashes.size()) { error = ERROR_INVALID_PARAMETER; return {}; }
        const std::uint64_t begin = std::uint64_t{index}*kPiece;
        const std::uint64_t end = std::min(kTotal, begin+kPiece);
        auto bytes = std::make_shared<std::vector<unsigned char>>(static_cast<std::size_t>(end-begin));
        std::uint64_t covered = 0;
        for (const auto& file : files) {
            const auto low = std::max(begin, file->offset);
            const auto high = std::min(end, file->offset+file->length);
            if (high <= low) continue;
            const std::wstring path = root+file->relative;
            Handle source(::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                        nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
            if (source.value == INVALID_HANDLE_VALUE) { error = ::GetLastError(); return {}; }
            const auto actual = final_path(source.value);
            if (actual.size() <= root.size() || !same(std::wstring_view(actual).substr(0,root.size()), root)) {
                error = ERROR_ACCESS_DENIED;
                return {};
            }
            LARGE_INTEGER length{};
            // Sparse/incomplete files need not have reached their final logical length yet. Require every byte
            // of this piece intersection to exist; its complete-piece hash still authenticates all returned data.
            if (!::GetFileSizeEx(source.value, &length) || length.QuadPart < 0 ||
                static_cast<std::uint64_t>(length.QuadPart) < high-file->offset ||
                static_cast<std::uint64_t>(length.QuadPart) > file->length) {
                error = ERROR_NOT_READY;
                return {};
            }
            if (!read_exact(source.value, low-file->offset, bytes->data()+static_cast<std::size_t>(low-begin), static_cast<DWORD>(high-low))) {
                error = ::GetLastError();
                return {};
            }
            covered += high-low;
        }
        if (covered != end-begin) { error = ERROR_INVALID_DATA; return {}; }
        std::array<unsigned char,20> hash{};
        if (!hash_bytes(BCRYPT_SHA1_ALGORITHM, *bytes, hash.data(), static_cast<ULONG>(hash.size()))) { error = ERROR_INVALID_FUNCTION; return {}; }
        if (hash != hashes[index]) { error = ERROR_NOT_READY; return {}; }
        retain(index, bytes);
        return bytes;
    }
};

State* state() {
    // Guest detach handlers may read late; retain the configured metadata/cache through process teardown.
    static State* const value = []() -> State* {
        try { return new State; }
        catch (const std::exception& e) { std::fprintf(stderr, "[rt] partial_package: configuration refused: %s\n", e.what()); return nullptr; }
    }();
    return value;
}

bool retryable(DWORD error) {
    return error == ERROR_NOT_READY || error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ||
           error == ERROR_SHARING_VIOLATION || error == ERROR_LOCK_VIOLATION || error == ERROR_HANDLE_EOF;
}
}  // namespace

bool enabled() { const auto* s = state(); return s && s->valid; }

FileRef find(std::wstring_view relative) {
    auto* s = state();
    if (!s || !s->valid) return {};
    try {
        std::wstring normalized;
        if (!relative_path(relative, normalized)) return {};
        for (const auto& f : s->files) if (same(f->relative, normalized)) return f;
    } catch (const std::bad_alloc&) {} catch (const std::length_error&) {}
    return {};
}

std::uint64_t size(const FileRef& file) { return file ? file->length : 0; }

bool read_at(const FileRef& file, std::uint64_t offset, void* destination, std::uint32_t length,
             std::uint32_t& read_bytes, DWORD& error) {
    read_bytes = 0;
    error = ERROR_SUCCESS;
    if (!file || !file->owner || !file->owner->valid) { error = ERROR_INVALID_HANDLE; return false; }
    if (!length || offset >= file->length) return true;
    if (length > kMaximumRead) { error = ERROR_NOT_SUPPORTED; return false; }
    const auto wanted = static_cast<std::uint32_t>(std::min<std::uint64_t>(length, file->length-offset));
    if (!writable(destination, wanted)) { error = ERROR_NOACCESS; return false; }
    try {
        const auto begin = file->offset+offset;
        const auto end = begin+wanted;
        const auto first = static_cast<std::uint32_t>(begin/kPiece);
        const auto last = static_cast<std::uint32_t>((end-1)/kPiece);
        std::vector<PieceRef> snapshots;
        snapshots.reserve(last-first+1);
        const ULONGLONG deadline = ::GetTickCount64()+10000;
        bool reported = false;
        for (auto index = first; index <= last; ++index) {
            PieceRef piece;
            while (!(piece = file->owner->load(index, error))) {
                if (!retryable(error)) return false;
                const auto now = ::GetTickCount64();
                if (now >= deadline) { error = ERROR_NOT_READY; return false; }
                if (!reported) {
                    static std::atomic<unsigned> messages{0};
                    if (messages.fetch_add(1, std::memory_order_relaxed) < 32)
                        std::fprintf(stderr, "[rt] partial_package: unverified piece %u, read offset %llu size %u; waiting at most10s\n",
                                     index, static_cast<unsigned long long>(offset), wanted);
                    reported = true;
                }
                ::Sleep(static_cast<DWORD>(std::min<ULONGLONG>(100, deadline-now)));
            }
            snapshots.push_back(std::move(piece));
        }
        // No source file is touched after validation. Cached snapshots are immutable even if the downloader
        // changes its file or resumes into another extent. Concurrent protection changes are the caller's race.
        if (!writable(destination, wanted)) { error = ERROR_NOACCESS; return false; }
        auto* out = static_cast<unsigned char*>(destination);
        for (std::uint32_t index = first; index <= last; ++index) {
            const auto piece_begin = std::uint64_t{index}*kPiece;
            const auto low = std::max(begin, piece_begin);
            const auto high = std::min(end, piece_begin+snapshots[index-first]->size());
            std::memcpy(out+static_cast<std::size_t>(low-begin), snapshots[index-first]->data()+static_cast<std::size_t>(low-piece_begin),
                        static_cast<std::size_t>(high-low));
        }
        read_bytes = wanted;
        error = ERROR_SUCCESS;
        return true;
    } catch (const std::bad_alloc&) { error = ERROR_NOT_ENOUGH_MEMORY; }
      catch (const std::length_error&) { error = ERROR_NOT_ENOUGH_MEMORY; }
    return false;
}
}  // namespace rt::partial_package
