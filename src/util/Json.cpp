#include "Json.hpp"

#include <cstdint>
#include <cstdio>

namespace {
    void skipWs(const std::string& s, size_t& i) {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r'))
            i++;
    }

    void appendUtf8(std::string& out, uint32_t cp) {
        if (cp < 0x80)
            out += static_cast<char>(cp);
        else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }

    bool hex4(const std::string& s, size_t pos, uint32_t& v) {
        if (pos + 4 > s.size())
            return false;
        v = 0;
        for (size_t k = 0; k < 4; k++) {
            const char H = s[pos + k];
            v <<= 4;
            if (H >= '0' && H <= '9')
                v |= H - '0';
            else if (H >= 'a' && H <= 'f')
                v |= H - 'a' + 10;
            else if (H >= 'A' && H <= 'F')
                v |= H - 'A' + 10;
            else
                return false;
        }
        return true;
    }

    // i at the opening quote; on success i is one past the closing quote
    bool parseString(const std::string& s, size_t& i, std::string& out) {
        if (i >= s.size() || s[i] != '"')
            return false;
        i++;
        out.clear();
        while (i < s.size()) {
            const char C = s[i];
            if (C == '"') {
                i++;
                return true;
            }
            if (C != '\\') {
                out += C;
                i++;
                continue;
            }
            i++;
            if (i >= s.size())
                return false;
            const char E = s[i++];
            switch (E) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    uint32_t cp = 0;
                    if (!hex4(s, i, cp))
                        return false;
                    i += 4;
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        // high surrogate: needs a \uDC00-\uDFFF right behind it
                        uint32_t lo = 0;
                        if (i + 1 < s.size() && s[i] == '\\' && s[i + 1] == 'u' && hex4(s, i + 2, lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                            i += 6;
                        } else
                            cp = 0xFFFD;
                    } else if (cp >= 0xDC00 && cp <= 0xDFFF)
                        cp = 0xFFFD; // lone low surrogate
                    appendUtf8(out, cp);
                    break;
                }
                default: return false;
            }
        }
        return false; // unterminated
    }

    // i at '{' or '['; skips the whole construct, ignoring braces inside strings
    bool skipBalanced(const std::string& s, size_t& i) {
        int  depth = 0;
        bool inStr = false;
        while (i < s.size()) {
            const char C = s[i];
            if (inStr) {
                if (C == '\\')
                    i++; // with the increment below: skip the escaped char
                else if (C == '"')
                    inStr = false;
            } else if (C == '"')
                inStr = true;
            else if (C == '{' || C == '[')
                depth++;
            else if (C == '}' || C == ']') {
                depth--;
                if (depth <= 0) {
                    i++;
                    return depth == 0;
                }
            }
            i++;
        }
        return false;
    }

    bool parseNumber(const std::string& s, size_t& i, std::string& out) {
        const size_t START  = i;
        bool         digits = false;
        if (i < s.size() && s[i] == '-')
            i++;
        while (i < s.size()) {
            const char C = s[i];
            if (C >= '0' && C <= '9') {
                digits = true;
                i++;
            } else if (C == '.' || C == 'e' || C == 'E' || C == '+' || C == '-')
                i++;
            else
                break;
        }
        if (!digits)
            return false;
        out = s.substr(START, i - START);
        return true;
    }
}

namespace MiniJson {
    std::optional<std::map<std::string, std::string>> parseObject(const std::string& s) {
        size_t i = 0;
        skipWs(s, i);
        if (i >= s.size() || s[i] != '{')
            return std::nullopt;
        i++;

        std::map<std::string, std::string> result;

        skipWs(s, i);
        if (i < s.size() && s[i] == '}')
            i++;
        else {
            while (true) {
                skipWs(s, i);
                std::string key;
                if (!parseString(s, i, key))
                    return std::nullopt;
                skipWs(s, i);
                if (i >= s.size() || s[i] != ':')
                    return std::nullopt;
                i++;
                skipWs(s, i);
                if (i >= s.size())
                    return std::nullopt;

                std::string val;
                const char  C = s[i];
                if (C == '"') {
                    if (!parseString(s, i, val))
                        return std::nullopt;
                } else if (C == '{' || C == '[') {
                    if (!skipBalanced(s, i)) // nested value recorded as ""
                        return std::nullopt;
                } else if (C == 't') {
                    if (s.compare(i, 4, "true") != 0)
                        return std::nullopt;
                    i += 4;
                    val = "true";
                } else if (C == 'f') {
                    if (s.compare(i, 5, "false") != 0)
                        return std::nullopt;
                    i += 5;
                    val = "false";
                } else if (C == 'n') {
                    if (s.compare(i, 4, "null") != 0) // null -> ""
                        return std::nullopt;
                    i += 4;
                } else if (C == '-' || (C >= '0' && C <= '9')) {
                    if (!parseNumber(s, i, val))
                        return std::nullopt;
                } else
                    return std::nullopt;

                result[key] = val;

                skipWs(s, i);
                if (i >= s.size())
                    return std::nullopt;
                if (s[i] == ',') {
                    i++;
                    continue;
                }
                if (s[i] == '}') {
                    i++;
                    break;
                }
                return std::nullopt;
            }
        }

        skipWs(s, i);
        if (i != s.size()) // trailing garbage
            return std::nullopt;
        return result;
    }

    std::string escape(const std::string& s) {
        std::string out;
        out.reserve(s.size() + 8);
        for (const unsigned char C : s) {
            switch (C) {
                case '"': out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n"; break;
                case '\t': out += "\\t"; break;
                case '\r': out += "\\r"; break;
                default:
                    if (C < 0x20) {
                        char buf[8];
                        std::snprintf(buf, sizeof(buf), "\\u%04x", C);
                        out += buf;
                    } else
                        out += static_cast<char>(C);
            }
        }
        return out;
    }
}
