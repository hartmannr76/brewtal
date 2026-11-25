#include "brew/network/json_scan.h"

#include <cstdlib>
#include <cstring>

namespace brew::net::json {

namespace {

constexpr int kMaxDepth = 32;

const char * ws(const char * p, const char * e)
{
    while(p < e && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
    return p;
}

/* p at the opening quote; returns just past the closing quote, or nullptr */
const char * skipString(const char * p, const char * e)
{
    if(p >= e || *p != '"') return nullptr;
    for(p++; p < e; p++) {
        if(*p == '\\') {
            if(++p >= e) return nullptr;
        }
        else if(*p == '"') {
            return p + 1;
        }
        else if(static_cast<unsigned char>(*p) < 0x20) {
            return nullptr;
        }
    }
    return nullptr;
}

const char * skipValue(const char * p, const char * e, int depth);

const char * skipContainer(const char * p, const char * e, int depth, char close, bool keyed)
{
    if(depth > kMaxDepth) return nullptr;
    p = ws(p + 1, e);
    if(p < e && *p == close) return p + 1;
    for(;;) {
        if(keyed) {
            p = skipString(p, e);
            if(!p) return nullptr;
            p = ws(p, e);
            if(p >= e || *p != ':') return nullptr;
            p = ws(p + 1, e);
        }
        p = skipValue(p, e, depth + 1);
        if(!p) return nullptr;
        p = ws(p, e);
        if(p >= e) return nullptr;
        if(*p == close) return p + 1;
        if(*p != ',') return nullptr;
        p = ws(p + 1, e);
    }
}

const char * skipValue(const char * p, const char * e, int depth)
{
    if(p >= e) return nullptr;
    switch(*p) {
        case '"': return skipString(p, e);
        case '{': return skipContainer(p, e, depth, '}', true);
        case '[': return skipContainer(p, e, depth, ']', false);
        case 't': return e - p >= 4 && std::strncmp(p, "true", 4) == 0 ? p + 4 : nullptr;
        case 'f': return e - p >= 5 && std::strncmp(p, "false", 5) == 0 ? p + 5 : nullptr;
        case 'n': return e - p >= 4 && std::strncmp(p, "null", 4) == 0 ? p + 4 : nullptr;
        default: break;
    }
    const char * s = p;
    if(p < e && *p == '-') p++;
    bool digits = false;
    while(p < e && ((*p >= '0' && *p <= '9') || *p == '.' || *p == 'e' || *p == 'E' || *p == '+' || *p == '-')) {
        digits = digits || (*p >= '0' && *p <= '9');
        p++;
    }
    return digits && p > s ? p : nullptr;
}

Value slice(const char * p, const char * end) { return end && end > p ? Value{ p, static_cast<size_t>(end - p) } : Value{}; }

int hex4(const char * p)
{
    int v = 0;
    for(int i = 0; i < 4; i++) {
        const char c = p[i];
        v <<= 4;
        if(c >= '0' && c <= '9') v |= c - '0';
        else if(c >= 'a' && c <= 'f') v |= c - 'a' + 10;
        else if(c >= 'A' && c <= 'F') v |= c - 'A' + 10;
        else return -1;
    }
    return v;
}

/* UTF-8 of code point `cp` into b; returns its length */
size_t utf8(unsigned long cp, char * b)
{
    if(cp < 0x80) { b[0] = static_cast<char>(cp); return 1; }
    if(cp < 0x800) { b[0] = static_cast<char>(0xC0 | (cp >> 6)); b[1] = static_cast<char>(0x80 | (cp & 0x3F)); return 2; }
    if(cp < 0x10000) {
        b[0] = static_cast<char>(0xE0 | (cp >> 12));
        b[1] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        b[2] = static_cast<char>(0x80 | (cp & 0x3F));
        return 3;
    }
    b[0] = static_cast<char>(0xF0 | (cp >> 18));
    b[1] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    b[2] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    b[3] = static_cast<char>(0x80 | (cp & 0x3F));
    return 4;
}

/* length of the UTF-8 character starting with byte c */
size_t utf8Len(unsigned char c) { return c < 0x80 ? 1 : c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1; }

}  // namespace

Value parse(const char * text, size_t len)
{
    if(!text) return {};
    const char * e = text + len;
    const char * p = ws(text, e);
    const char * end = skipValue(p, e, 0);
    if(!end || ws(end, e) != e) return {};
    return slice(p, end);
}

Value get(Value o, const char * key)
{
    if(!isObject(o) || !key) return {};
    const char * e = o.p + o.n;
    const size_t kn = std::strlen(key);
    const char * p = ws(o.p + 1, e);
    while(p < e && *p == '"') {
        const char * kend = skipString(p, e);
        if(!kend) return {};
        const bool match = static_cast<size_t>(kend - p - 2) == kn && std::strncmp(p + 1, key, kn) == 0;
        p = ws(kend, e);
        if(p >= e || *p != ':') return {};
        p = ws(p + 1, e);
        const char * vend = skipValue(p, e, 1);
        if(!vend) return {};
        if(match) return slice(p, vend);
        p = ws(vend, e);
        if(p < e && *p == ',') p = ws(p + 1, e);
    }
    return {};
}

Value at(Value a, size_t i)
{
    if(!isArray(a)) return {};
    const char * e = a.p + a.n;
    const char * p = ws(a.p + 1, e);
    for(size_t k = 0; p < e && *p != ']'; k++) {
        const char * vend = skipValue(p, e, 1);
        if(!vend) return {};
        if(k == i) return slice(p, vend);
        p = ws(vend, e);
        if(p < e && *p == ',') p = ws(p + 1, e);
    }
    return {};
}

size_t size(Value c)
{
    const bool obj = isObject(c);
    if(!obj && !isArray(c)) return 0;
    const char * e = c.p + c.n;
    const char * p = ws(c.p + 1, e);
    size_t k = 0;
    while(p < e && *p != (obj ? '}' : ']')) {
        if(obj) {
            p = skipString(p, e);
            if(!p) return k;
            p = ws(p, e);
            if(p >= e || *p != ':') return k;
            p = ws(p + 1, e);
        }
        const char * vend = skipValue(p, e, 1);
        if(!vend) return k;
        k++;
        p = ws(vend, e);
        if(p < e && *p == ',') p = ws(p + 1, e);
    }
    return k;
}

bool isNull(Value v) { return v.n == 4 && std::strncmp(v.p, "null", 4) == 0; }
bool isString(Value v) { return v.n >= 2 && v.p[0] == '"'; }
bool isArray(Value v) { return v.n >= 2 && v.p[0] == '['; }
bool isObject(Value v) { return v.n >= 2 && v.p[0] == '{'; }
bool isNumber(Value v) { return v.n > 0 && (v.p[0] == '-' || (v.p[0] >= '0' && v.p[0] <= '9')); }

double number(Value v, double fallback)
{
    if(!isNumber(v) || v.n > 40) return fallback;
    char b[41];
    std::memcpy(b, v.p, v.n);
    b[v.n] = '\0';
    char * end = nullptr;
    const double d = std::strtod(b, &end);
    return end == b ? fallback : d;
}

bool string(Value v, char * out, size_t n)
{
    if(!out || n == 0) return false;
    out[0] = '\0';
    if(!isString(v)) return false;
    size_t o = 0;
    const char * p = v.p + 1;
    const char * e = v.p + v.n - 1;   // the closing quote
    while(p < e) {
        char b[4];
        size_t bl = 0;
        if(*p != '\\') {   /* a raw character: copy it whole (multi-byte UTF-8 too) */
            bl = utf8Len(static_cast<unsigned char>(*p));
            if(p + bl > e) bl = static_cast<size_t>(e - p);
            std::memcpy(b, p, bl);
            p += bl;
        }
        else {
            const char c = p[1];
            p += 2;
            switch(c) {
                case 'n': b[0] = '\n'; bl = 1; break;
                case 't': b[0] = '\t'; bl = 1; break;
                case 'r': b[0] = '\r'; bl = 1; break;
                case 'b': b[0] = '\b'; bl = 1; break;
                case 'f': b[0] = '\f'; bl = 1; break;
                case 'u': {
                    long cp = e - p >= 4 ? hex4(p) : -1;
                    if(cp < 0) return out[o] = '\0', true;
                    p += 4;
                    if(cp >= 0xD800 && cp < 0xDC00 && e - p >= 6 && p[0] == '\\' && p[1] == 'u') {   // surrogate pair
                        const long lo = hex4(p + 2);
                        if(lo >= 0xDC00 && lo < 0xE000) {
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                            p += 6;
                        }
                    }
                    bl = utf8(static_cast<unsigned long>(cp), b);
                    break;
                }
                default: b[0] = c; bl = 1; break;   // \" \\ \/
            }
        }
        if(o + bl >= n) break;   // no room for the whole character: stop before it
        std::memcpy(out + o, b, bl);
        o += bl;
    }
    out[o] = '\0';
    return true;
}

bool equals(Value v, const char * s)
{
    if(!isString(v) || !s) return false;
    char b[128];
    if(v.n > sizeof(b) + 2) return false;
    string(v, b, sizeof(b));
    return std::strcmp(b, s) == 0;
}

}  // namespace brew::net::json
