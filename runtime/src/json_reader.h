#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace leonida {

struct JsonObjectEntry;

struct Json {
    enum class Kind { null, boolean, number, string, array, object } kind = Kind::null;
    bool boolean = false;
    double number = 0;
    std::string string;
    std::vector<Json> array;
    // A forward-declared element permits recursive values. Implicit special members keep subtree moves noexcept.
    std::vector<JsonObjectEntry> object;

    const Json* find(std::string_view key) const;
};

struct JsonObjectEntry {
    std::string key;
    Json value;
};

inline const Json* Json::find(std::string_view key) const {
    for (const auto& [k, v] : object)
        if (k == key) return &v;
    return nullptr;
}

class JsonReader {
public:
    explicit JsonReader(std::string_view text) : text_(text) {}

    bool parse(Json& out);

private:
    void space();
    bool literal(std::string_view word);
    bool value(Json& out, int depth);
    static int hex_digit(char c);
    bool string(std::string& out);

    std::string_view text_;
    std::size_t pos_ = 0;
};

}  // namespace leonida

// Standalone clients can include the reader without linking the runtime's CMake library.
#ifndef LEONIDA_COMPILED_JSON
#define LEONIDA_JSON_INLINE inline
#include "json_reader_impl.inl"
#undef LEONIDA_JSON_INLINE
#endif
