// net.cpp — HTTP и JSON для виджетов
#include "net.h"
#include <winhttp.h>
#include <cstring>
#include <cstdlib>

namespace net {

std::string ToU8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}
std::wstring FromU8(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}
std::wstring UrlEnc(const std::wstring& w) {
    std::string s = ToU8(w);
    std::wstring o;
    static const char* hex = "0123456789ABCDEF";
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o += (wchar_t)c;
        else { o += L'%'; o += (wchar_t)hex[c >> 4]; o += (wchar_t)hex[c & 15]; }
    }
    return o;
}

static void SkipWs(const char*& p, const char* e) {
    while (p < e && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) p++;
}
static bool Hex4(const char*& p, const char* e, unsigned& v) {
    if (e - p < 4) return false;
    v = 0;
    for (int i = 0; i < 4; i++) {
        char c = *p++;
        v <<= 4;
        if (c >= '0' && c <= '9') v |= c - '0';
        else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
        else return false;
    }
    return true;
}
static void AppendU8(std::string& o, unsigned cp) {
    if (cp < 0x80) o += (char)cp;
    else if (cp < 0x800) { o += (char)(0xC0 | (cp >> 6)); o += (char)(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) {
        o += (char)(0xE0 | (cp >> 12)); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F));
    } else {
        o += (char)(0xF0 | (cp >> 18)); o += (char)(0x80 | ((cp >> 12) & 0x3F));
        o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F));
    }
}
static bool ParseStr(const char*& p, const char* e, std::string& out) {
    if (p >= e || *p != '"') return false;
    p++;
    while (p < e) {
        char c = *p++;
        if (c == '"') return true;
        if (c != '\\') { out += c; continue; }
        if (p >= e) return false;
        char x = *p++;
        switch (x) {
            case 'n': out += '\n'; break;
            case 't': out += '\t'; break;
            case 'r': out += '\r'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'u': {
                unsigned cp = 0;
                if (!Hex4(p, e, cp)) return false;
                if (cp >= 0xD800 && cp <= 0xDBFF && e - p >= 6 && p[0] == '\\' && p[1] == 'u') {
                    p += 2;
                    unsigned lo = 0;
                    if (!Hex4(p, e, lo)) return false;
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                }
                AppendU8(out, cp);
                break;
            }
            default: out += x;
        }
    }
    return false;
}
static bool ParseVal(const char*& p, const char* e, JVal& v, int depth) {
    if (depth > 64) return false;
    SkipWs(p, e);
    if (p >= e) return false;
    char c = *p;
    if (c == '{') {
        v.t = JVal::Obj;
        p++;
        SkipWs(p, e);
        if (p < e && *p == '}') { p++; return true; }
        for (;;) {
            SkipWs(p, e);
            std::string k;
            if (!ParseStr(p, e, k)) return false;
            SkipWs(p, e);
            if (p >= e || *p != ':') return false;
            p++;
            JVal val;
            if (!ParseVal(p, e, val, depth + 1)) return false;
            v.o.emplace_back(std::move(k), std::move(val));
            SkipWs(p, e);
            if (p < e && *p == ',') { p++; continue; }
            if (p < e && *p == '}') { p++; return true; }
            return false;
        }
    }
    if (c == '[') {
        v.t = JVal::Arr;
        p++;
        SkipWs(p, e);
        if (p < e && *p == ']') { p++; return true; }
        for (;;) {
            JVal val;
            if (!ParseVal(p, e, val, depth + 1)) return false;
            v.a.push_back(std::move(val));
            SkipWs(p, e);
            if (p < e && *p == ',') { p++; continue; }
            if (p < e && *p == ']') { p++; return true; }
            return false;
        }
    }
    if (c == '"') { v.t = JVal::Str; return ParseStr(p, e, v.s); }
    if (e - p >= 4 && !strncmp(p, "true", 4)) { v.t = JVal::Bool; v.b = true; p += 4; return true; }
    if (e - p >= 5 && !strncmp(p, "false", 5)) { v.t = JVal::Bool; v.b = false; p += 5; return true; }
    if (e - p >= 4 && !strncmp(p, "null", 4)) { v.t = JVal::Null; p += 4; return true; }
    const char* s = p;
    while (p < e && (strchr("+-.eE", *p) || (*p >= '0' && *p <= '9'))) p++;
    if (p == s) return false;
    v.t = JVal::Num;
    v.n = strtod(std::string(s, p).c_str(), nullptr);
    return true;
}
bool ParseJson(const std::string& s, JVal& v) {
    const char* p = s.data();
    return ParseVal(p, s.data() + s.size(), v, 0);
}


bool HttpGet(const std::wstring& url, DWORD& status, std::string& body, DWORD timeoutMs) {
    status = 0;
    body.clear();
    URL_COMPONENTS uc = {};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256] = {}, path[4096] = {};
    uc.lpszHostName = host;
    uc.dwHostNameLength = 255;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 4095;
    wchar_t extra[4096] = {};
    uc.lpszExtraInfo = extra;
    uc.dwExtraInfoLength = 4095;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) return false;
    std::wstring full = std::wstring(path) + extra;
    if (full.empty()) full = L"/";
    const wchar_t* ua = L"DynamicIsland-Windows/0.6 (derk)";
    HINTERNET ses = WinHttpOpen(ua, 4 /*AUTOMATIC_PROXY*/, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) ses = WinHttpOpen(ua, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) return false;
    WinHttpSetTimeouts(ses, (int)timeoutMs, (int)timeoutMs, (int)timeoutMs, (int)timeoutMs);
    DWORD redir = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
    WinHttpSetOption(ses, WINHTTP_OPTION_REDIRECT_POLICY, &redir, sizeof(redir));
    bool ok = false;
    HINTERNET con = WinHttpConnect(ses, host, uc.nPort, 0);
    if (con) {
        HINTERNET r = WinHttpOpenRequest(con, L"GET", full.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                         uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
        if (r) {
            if (WinHttpSendRequest(r, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                WinHttpReceiveResponse(r, nullptr)) {
                DWORD len = sizeof(status);
                WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                                    &status, &len, WINHTTP_NO_HEADER_INDEX);
                for (;;) {
                    DWORD avail = 0;
                    if (!WinHttpQueryDataAvailable(r, &avail) || avail == 0) break;
                    size_t old = body.size();
                    body.resize(old + avail);
                    DWORD rd = 0;
                    if (!WinHttpReadData(r, &body[old], avail, &rd)) { body.resize(old); break; }
                    body.resize(old + rd);
                    if (body.size() > 16 * 1024 * 1024) break;
                }
                ok = status != 0;
            }
            WinHttpCloseHandle(r);
        }
        WinHttpCloseHandle(con);
    }
    WinHttpCloseHandle(ses);
    return ok;
}

}  // namespace net
