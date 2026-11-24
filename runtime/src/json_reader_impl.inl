// The implementation used by json_reader.cpp and by standalone header clients.
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <utility>

namespace leonida {

LEONIDA_JSON_INLINE bool JsonReader::parse(Json& out) {
    if (!value(out, 0)) return false;
    space();
    return pos_ == text_.size();
}

LEONIDA_JSON_INLINE void JsonReader::space() {
    while (pos_ < text_.size() && (text_[pos_] == ' ' || text_[pos_] == '\n' || text_[pos_] == '\r' ||
                                   text_[pos_] == '\t'))
        ++pos_;
}

LEONIDA_JSON_INLINE bool JsonReader::literal(std::string_view word) {
    if (text_.substr(pos_, word.size()) != word) return false;
    pos_ += word.size();
    return true;
}

LEONIDA_JSON_INLINE bool JsonReader::value(Json& out, int depth) {
    if (depth > 64) return false;
    space();
    if (pos_ >= text_.size()) return false;
    const char c = text_[pos_];
    if (c == '{') {
        out.kind = Json::Kind::object;
        ++pos_;
        space();
        if (pos_ < text_.size() && text_[pos_] == '}') return ++pos_, true;
        for (;;) {
            space();
            std::string key;
            if (!string(key)) return false;
            space();
            if (pos_ >= text_.size() || text_[pos_++] != ':') return false;
            Json item;
            if (!value(item, depth + 1)) return false;
            out.object.emplace_back(std::move(key), std::move(item));
            space();
            if (pos_ >= text_.size()) return false;
            if (text_[pos_] == ',') {
                ++pos_;
                continue;
            }
            return text_[pos_++] == '}';
        }
    }
    if (c == '[') {
        out.kind = Json::Kind::array;
        ++pos_;
        space();
        if (pos_ < text_.size() && text_[pos_] == ']') return ++pos_, true;
        for (;;) {
            Json item;
            if (!value(item, depth + 1)) return false;
            out.array.push_back(std::move(item));
            space();
            if (pos_ >= text_.size()) return false;
            if (text_[pos_] == ',') {
                ++pos_;
                continue;
            }
            return text_[pos_++] == ']';
        }
    }
    if (c == '"') {
        out.kind = Json::Kind::string;
        return string(out.string);
    }
    if (literal("true")) return out.kind = Json::Kind::boolean, out.boolean = true, true;
    if (literal("false")) return out.kind = Json::Kind::boolean, out.boolean = false, true;
    if (literal("null")) return out.kind = Json::Kind::null, true;
    // Numbers through strtod: Apple's libc++ does not provide the floating-point from_chars overload. The
    // token is already limited to JSON number characters, and the end check requires all of it to be consumed.
    const std::size_t start = pos_;
    while (pos_ < text_.size() && std::strchr("+-0123456789.eE", text_[pos_])) ++pos_;
    if (pos_ == start) return false;
    out.kind = Json::Kind::number;
    const std::string token(text_.substr(start, pos_ - start));
    char* end = nullptr;
    errno = 0;
    out.number = std::strtod(token.c_str(), &end);
    return errno != ERANGE && end == token.c_str() + token.size();
}

LEONIDA_JSON_INLINE int JsonReader::hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

LEONIDA_JSON_INLINE bool JsonReader::string(std::string& out) {
    if (pos_ >= text_.size() || text_[pos_++] != '"') return false;
    while (pos_ < text_.size()) {
        const char c = text_[pos_++];
        if (c == '"') return true;
        if (c != '\\') {
            out.push_back(c);
            continue;
        }
        if (pos_ >= text_.size()) return false;
        const char e = text_[pos_++];
        switch (e) {
        case '"': case '\\': case '/': out.push_back(e); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        case 'u': {
            if (pos_ + 4 > text_.size()) return false;
            unsigned code = 0;
            for (int i = 0; i < 4; ++i) {  // exactly four hex digits
                const int digit = hex_digit(text_[pos_++]);
                if (digit < 0) return false;
                code = code << 4 | unsigned(digit);
            }
            if (code < 0x80) {
                out.push_back(char(code));
            } else if (code < 0x800) {
                out.push_back(char(0xc0 | (code >> 6)));
                out.push_back(char(0x80 | (code & 0x3f)));
            } else {  // surrogate pairs are kept as two 3-byte sequences; index names are ASCII
                out.push_back(char(0xe0 | (code >> 12)));
                out.push_back(char(0x80 | ((code >> 6) & 0x3f)));
                out.push_back(char(0x80 | (code & 0x3f)));
            }
            break;
        }
        default: return false;
        }
    }
    return false;
}

}  // namespace leonida
