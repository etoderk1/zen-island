// texttools.cpp
#include "texttools.h"
#include <windows.h>
#include <algorithm>
#include <vector>
#include <set>
#include <cwctype>

const wchar_t* TextOpName(int op) {
    static const wchar_t* n[TT_COUNT] = {L"Раскладка",      L"ВЕРХНИЙ",        L"нижний",       L"Каждое Слово",
                                         L"Предложение",    L"Транслит",       L"Убрать переносы", L"Лишние пробелы",
                                         L"JSON красиво",   L"JSON в строку",  L"→ Base64",     L"Base64 →",
                                         L"→ URL",          L"URL →",          L"Сортировать строки", L"Без повторов"};
    return op >= 0 && op < TT_COUNT ? n[op] : L"";
}

static std::wstring Upper(std::wstring s) { if (!s.empty()) CharUpperBuffW(&s[0], (DWORD)s.size()); return s; }
static std::wstring Lower(std::wstring s) { if (!s.empty()) CharLowerBuffW(&s[0], (DWORD)s.size()); return s; }
static wchar_t Up(wchar_t c) { CharUpperBuffW(&c, 1); return c; }
static bool IsLetter(wchar_t c) { return IsCharAlphaW(c) != 0; }
static std::string U8(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    if (n) WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}
static bool W(const std::string& s, std::wstring& out) {
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), (int)s.size(), nullptr, 0);
    if (n <= 0 && !s.empty()) return false;
    out.assign(n, L'\0');
    if (n) MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &out[0], n);
    return true;
}

// ЙЦУКЕН <-> QWERTY
static const wchar_t* kEn = L"`qwertyuiop[]asdfghjkl;'zxcvbnm,./~QWERTYUIOP{}ASDFGHJKL:\"ZXCVBNM<>?@#$^&";
static const wchar_t* kRu = L"ёйцукенгшщзхъфывапролджэячсмитьбю.ЁЙЦУКЕНГШЩЗХЪФЫВАПРОЛДЖЭЯЧСМИТЬБЮ,\"№;:?";
static std::wstring SwapLayout(const std::wstring& s) {
    // в какую сторону: чего в тексте больше — латиницы или кириллицы
    int lat = 0, cyr = 0;
    for (wchar_t c : s) { if (c >= 0x400 && c <= 0x4FF) cyr++; else if (c < 128 && iswalpha(c)) lat++; }
    const wchar_t* from = lat >= cyr ? kEn : kRu;
    const wchar_t* to = lat >= cyr ? kRu : kEn;
    std::wstring r = s;
    size_t n = wcslen(from);
    for (auto& c : r)
        for (size_t i = 0; i < n; i++)
            if (from[i] == c) { c = to[i]; break; }
    return r;
}
static std::wstring Translit(const std::wstring& s) {
    static const wchar_t* lo[33] = {L"a", L"b", L"v", L"g", L"d", L"e", L"zh", L"z", L"i", L"y", L"k", L"l", L"m", L"n", L"o", L"p", L"r",
                                    L"s", L"t", L"u", L"f", L"kh", L"ts", L"ch", L"sh", L"shch", L"", L"y", L"", L"e", L"yu", L"ya", L"yo"};
    std::wstring r;
    for (wchar_t c : s) {
        int idx = -1;
        bool up = false;
        if (c >= 0x430 && c <= 0x44F) idx = c - 0x430;
        else if (c >= 0x410 && c <= 0x42F) { idx = c - 0x410; up = true; }
        else if (c == 0x451) idx = 32;
        else if (c == 0x401) { idx = 32; up = true; }
        if (idx < 0) { r += c; continue; }
        std::wstring t = lo[idx];
        if (up && !t.empty()) t[0] = (wchar_t)towupper(t[0]);
        r += t;
    }
    return r;
}
static std::wstring TitleCase(const std::wstring& s) {
    std::wstring r = Lower(s);
    bool start = true;
    for (auto& c : r) {
        if (IsLetter(c)) { if (start) c = Up(c); start = false; }
        else start = iswspace(c) || c == L'-' || c == L'"' || c == L'(' || c == L'«';
    }
    return r;
}
static std::wstring SentenceCase(const std::wstring& s) {
    std::wstring r = Lower(s);
    bool start = true;
    for (auto& c : r) {
        if (IsLetter(c)) { if (start) c = Up(c); start = false; }
        else if (c == L'.' || c == L'!' || c == L'?' || c == L'\n') start = true;
    }
    return r;
}
static std::wstring JoinLines(const std::wstring& s) {
    std::wstring r;
    for (size_t i = 0; i < s.size(); i++) {
        wchar_t c = s[i];
        if (c == L'\r') continue;
        if (c == L'\n') {
            // пустая строка = абзац, сохраняем
            size_t j = i + 1;
            while (j < s.size() && (s[j] == L'\r' || s[j] == L' ' || s[j] == L'\t')) j++;
            if (j < s.size() && s[j] == L'\n') { r += L"\r\n\r\n"; i = j; continue; }
            if (!r.empty() && r.back() == L'-') { r.pop_back(); continue; }  // перенос слова
            if (!r.empty() && r.back() != L' ') r += L' ';
            continue;
        }
        r += c;
    }
    return r;
}
static std::wstring Spaces(const std::wstring& s) {
    std::wstring r;
    for (wchar_t c : s) {
        if (c == L'\t') c = L' ';
        if (c == L' ' && !r.empty() && r.back() == L' ') continue;
        if ((c == L'\n' || c == L'\r') && !r.empty() && r.back() == L' ') r.pop_back();
        if (c == L' ' && !r.empty() && r.back() == L'\n') continue;
        r += c;
    }
    size_t a = r.find_first_not_of(L" \r\n"), b = r.find_last_not_of(L" \r\n");
    return a == std::wstring::npos ? L"" : r.substr(a, b - a + 1);
}
// JSON: переформатируем токены, внутри строк ничего не трогаем
static bool JsonFormat(const std::wstring& s, bool pretty, std::wstring& out, std::wstring& err) {
    out.clear();
    int depth = 0;
    bool inStr = false, esc = false, any = false;
    std::vector<wchar_t> stack;
    auto nl = [&](int d) { out += L"\r\n"; out.append(d * 2, L' '); };
    for (size_t i = 0; i < s.size(); i++) {
        wchar_t c = s[i];
        if (inStr) {
            out += c;
            if (esc) esc = false;
            else if (c == L'\\') esc = true;
            else if (c == L'"') inStr = false;
            continue;
        }
        if (iswspace(c)) continue;
        any = true;
        switch (c) {
            case L'"': inStr = true; out += c; break;
            case L'{': case L'[': {
                stack.push_back(c);
                out += c;
                size_t j = i + 1;
                while (j < s.size() && iswspace(s[j])) j++;
                if (j < s.size() && (s[j] == L'}' || s[j] == L']')) break;  // пустой объект
                depth++;
                if (pretty) nl(depth);
                break;
            }
            case L'}': case L']': {
                if (stack.empty() || (c == L'}' && stack.back() != L'{') || (c == L']' && stack.back() != L'[')) {
                    err = L"Скобки не сходятся";
                    return false;
                }
                stack.pop_back();
                if (!out.empty() && (out.back() == L'{' || out.back() == L'[')) { out += c; break; }
                depth--;
                if (pretty) nl(depth);
                out += c;
                break;
            }
            case L',': out += c; if (pretty) nl(depth); break;
            case L':': out += pretty ? L": " : L":"; break;
            default: out += c;
        }
    }
    if (!any) { err = L"Пусто"; return false; }
    if (inStr || !stack.empty()) { err = L"Это не JSON"; return false; }
    return true;
}
static const char* kB64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static std::wstring B64Enc(const std::wstring& w) {
    std::string s = U8(w), o;
    for (size_t i = 0; i < s.size(); i += 3) {
        unsigned v = (unsigned char)s[i] << 16;
        if (i + 1 < s.size()) v |= (unsigned char)s[i + 1] << 8;
        if (i + 2 < s.size()) v |= (unsigned char)s[i + 2];
        o += kB64[(v >> 18) & 63];
        o += kB64[(v >> 12) & 63];
        o += i + 1 < s.size() ? kB64[(v >> 6) & 63] : '=';
        o += i + 2 < s.size() ? kB64[v & 63] : '=';
    }
    return std::wstring(o.begin(), o.end());
}
static bool B64Dec(const std::wstring& w, std::wstring& out) {
    std::string o;
    unsigned v = 0;
    int bits = 0;
    for (wchar_t c : w) {
        if (iswspace(c) || c == L'=') continue;
        int x;
        if (c == L'-') x = 62; else if (c == L'_') x = 63;  // base64url
        else {
            const char* p = c < 128 ? strchr(kB64, (char)c) : nullptr;
            if (!p || !c) return false;
            x = (int)(p - kB64);
        }
        v = (v << 6) | x;
        bits += 6;
        if (bits >= 8) { bits -= 8; o += (char)((v >> bits) & 0xFF); }
    }
    return W(o, out);
}
static std::wstring UrlEnc(const std::wstring& w) {
    std::string s = U8(w);
    std::wstring o;
    static const char* hex = "0123456789ABCDEF";
    for (unsigned char c : s) {
        if (isalnum(c) || strchr("-_.~", c)) o += (wchar_t)c;
        else { o += L'%'; o += (wchar_t)hex[c >> 4]; o += (wchar_t)hex[c & 15]; }
    }
    return o;
}
static bool UrlDec(const std::wstring& w, std::wstring& out) {
    std::string s;
    for (size_t i = 0; i < w.size(); i++) {
        if (w[i] == L'%' && i + 2 < w.size() && iswxdigit(w[i + 1]) && iswxdigit(w[i + 2])) {
            s += (char)wcstoul(w.substr(i + 1, 2).c_str(), nullptr, 16);
            i += 2;
        } else if (w[i] == L'+') s += ' ';
        else s += U8(std::wstring(1, w[i]));
    }
    return W(s, out);
}
static std::vector<std::wstring> Lines(const std::wstring& s) {
    std::vector<std::wstring> v;
    size_t a = 0;
    while (a <= s.size()) {
        size_t e = s.find(L'\n', a);
        std::wstring l = s.substr(a, e == std::wstring::npos ? std::wstring::npos : e - a);
        if (!l.empty() && l.back() == L'\r') l.pop_back();
        v.push_back(l);
        if (e == std::wstring::npos) break;
        a = e + 1;
    }
    return v;
}
static std::wstring JoinV(const std::vector<std::wstring>& v) {
    std::wstring r;
    for (size_t i = 0; i < v.size(); i++) { if (i) r += L"\r\n"; r += v[i]; }
    return r;
}

bool ApplyTextOp(int op, const std::wstring& in, std::wstring& out, std::wstring& err) {
    err.clear();
    switch (op) {
        case TT_LAYOUT: out = SwapLayout(in); return true;
        case TT_UPPER: out = Upper(in); return true;
        case TT_LOWER: out = Lower(in); return true;
        case TT_TITLE: out = TitleCase(in); return true;
        case TT_SENTENCE: out = SentenceCase(in); return true;
        case TT_TRANSLIT: out = Translit(in); return true;
        case TT_JOINLINES: out = JoinLines(in); return true;
        case TT_SPACES: out = Spaces(in); return true;
        case TT_JSON_PRETTY: return JsonFormat(in, true, out, err);
        case TT_JSON_MIN: return JsonFormat(in, false, out, err);
        case TT_B64_ENC: out = B64Enc(in); return true;
        case TT_B64_DEC: if (B64Dec(in, out)) return true; err = L"Это не Base64 (или не текст)"; return false;
        case TT_URL_ENC: out = UrlEnc(in); return true;
        case TT_URL_DEC: if (UrlDec(in, out)) return true; err = L"Не получилось раскодировать"; return false;
        case TT_SORT_LINES: {
            auto v = Lines(in);
            std::sort(v.begin(), v.end(), [](const std::wstring& a, const std::wstring& b) { return Lower(a) < Lower(b); });
            out = JoinV(v);
            return true;
        }
        case TT_UNIQUE_LINES: {
            auto v = Lines(in);
            std::vector<std::wstring> r;
            std::set<std::wstring> seen;
            for (auto& l : v) if (seen.insert(l).second) r.push_back(l);
            out = JoinV(r);
            return true;
        }
    }
    out = in;
    return true;
}
