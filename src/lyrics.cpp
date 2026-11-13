// lyrics.cpp — LRCLIB (https://lrclib.net): поиск, кэш, разбор LRC
#include "lyrics.h"
#include <winhttp.h>
#include <shlobj.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwctype>

// ---------------------------------------------------------------------------
// UTF-8 <-> UTF-16
// ---------------------------------------------------------------------------
static std::string ToU8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}
static std::wstring FromU8(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}
static std::wstring Lower(std::wstring s) {
    if (!s.empty()) CharLowerBuffW(&s[0], (DWORD)s.size());  // понимает кириллицу, в отличие от towlower
    return s;
}
static std::wstring Trim(const std::wstring& s) {
    size_t a = 0, b = s.size();
    while (a < b && iswspace(s[a])) a++;
    while (b > a && iswspace(s[b - 1])) b--;
    return s.substr(a, b - a);
}

// ---------------------------------------------------------------------------
// Мини-JSON (ровно сколько нужно для ответов LRCLIB)
// ---------------------------------------------------------------------------
struct JVal {
    enum T { Null, Bool, Num, Str, Arr, Obj } t = Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<JVal> a;
    std::vector<std::pair<std::string, JVal>> o;
    const JVal* Get(const char* k) const {
        for (auto& kv : o)
            if (kv.first == k) return &kv.second;
        return nullptr;
    }
};
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
static bool ParseJson(const std::string& s, JVal& v) {
    const char* p = s.data();
    return ParseVal(p, s.data() + s.size(), v, 0);
}

// ---------------------------------------------------------------------------
// LRC
// ---------------------------------------------------------------------------
// "mm:ss.xx" / "mm:ss" / "mm:ss:xx" -> секунды; p указывает после '[' или '<'
static bool ParseStamp(const std::wstring& s, size_t& i, wchar_t close, double& t) {
    size_t j = s.find(close, i);
    if (j == std::wstring::npos) return false;
    std::wstring body = s.substr(i, j - i);
    int mm = 0;
    double ss = 0;
    size_t colon = body.find(L':');
    if (colon == std::wstring::npos || colon == 0) return false;
    for (size_t k = 0; k < colon; k++)
        if (!iswdigit(body[k])) return false;
    mm = _wtoi(body.substr(0, colon).c_str());
    std::wstring rest = body.substr(colon + 1);
    if (rest.empty() || !iswdigit(rest[0])) return false;
    size_t c2 = rest.find(L':');  // формат mm:ss:xx
    if (c2 != std::wstring::npos) rest[c2] = L'.';
    for (wchar_t ch : rest)
        if (!iswdigit(ch) && ch != L'.') return false;
    ss = _wtof(rest.c_str());
    t = mm * 60.0 + ss;
    i = j + 1;
    return true;
}

static void SplitWords(LyricLine& ln) {
    ln.words.clear();
    const std::wstring& s = ln.text;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && iswspace(s[i])) i++;
        size_t st = i;
        while (i < s.size() && !iswspace(s[i])) i++;
        if (i > st) ln.words.push_back({-1, (int)st, (int)(i - st)});
    }
}

bool ParseLrc(const std::wstring& lrc, Lyrics& out) {
    out.lines.clear();
    double offset = 0;
    size_t pos = 0;
    while (pos <= lrc.size()) {
        size_t nl = lrc.find(L'\n', pos);
        std::wstring line = lrc.substr(pos, nl == std::wstring::npos ? std::wstring::npos : nl - pos);
        pos = nl == std::wstring::npos ? lrc.size() + 1 : nl + 1;
        if (!line.empty() && line.back() == L'\r') line.pop_back();

        size_t i = 0;
        while (i < line.size() && iswspace(line[i])) i++;
        std::vector<double> times;
        while (i < line.size() && line[i] == L'[') {
            size_t save = i;
            i++;
            double t;
            if (ParseStamp(line, i, L']', t)) { times.push_back(t); continue; }
            // метаданные вида [offset:+200]
            i = save;
            size_t j = line.find(L']', i);
            if (j == std::wstring::npos) break;
            std::wstring tag = Lower(line.substr(i + 1, j - i - 1));
            if (tag.rfind(L"offset:", 0) == 0) offset = _wtof(tag.c_str() + 7) / 1000.0;
            i = j + 1;
            break;
        }
        if (times.empty()) continue;

        // текст + пословные метки <mm:ss.xx>
        LyricLine ln;
        std::wstring text;
        std::vector<LyricWord> words;
        double pendingT = -1;
        while (i < line.size()) {
            if (line[i] == L'<') {
                size_t k = i + 1;
                double t;
                if (ParseStamp(line, k, L'>', t)) { pendingT = t; i = k; continue; }
            }
            if (pendingT >= 0 && !iswspace(line[i])) {
                words.push_back({pendingT, (int)text.size(), 0});
                pendingT = -1;
            }
            text += line[i++];
        }
        // обрезаем пробелы, сдвигая индексы слов
        size_t lead = 0;
        while (lead < text.size() && iswspace(text[lead])) lead++;
        std::wstring trimmed = Trim(text);
        for (auto& w : words) w.start = std::max(0, w.start - (int)lead);
        ln.text = trimmed;
        if (words.size() >= 2) {
            for (size_t k = 0; k < words.size(); k++) {
                int end = k + 1 < words.size() ? words[k + 1].start : (int)trimmed.size();
                while (end > words[k].start && end - 1 < (int)trimmed.size() && iswspace(trimmed[end - 1])) end--;
                words[k].len = std::max(0, end - words[k].start);
            }
            ln.words = words;
            ln.wordTimes = true;
        } else {
            SplitWords(ln);
        }
        for (double t : times) {
            LyricLine c = ln;
            c.t = std::max(0.0, t - offset);
            out.lines.push_back(std::move(c));
        }
    }
    std::stable_sort(out.lines.begin(), out.lines.end(), [](const LyricLine& a, const LyricLine& b) { return a.t < b.t; });
    bool anyText = false;
    for (auto& l : out.lines)
        if (!l.text.empty()) { anyText = true; break; }
    if (!anyText) out.lines.clear();
    return !out.lines.empty();
}

// ---------------------------------------------------------------------------
// Нормализация названий (YouTube и т.п.)
// ---------------------------------------------------------------------------
static std::wstring StripJunkBrackets(const std::wstring& s) {
    static const wchar_t* junk[] = {L"official", L"video", L"audio", L"lyric", L"visualizer", L"visualiser",
                                    L"music video", L"mv", L"hd", L"4k", L"remaster", L"клип", L"премьера",
                                    L"live", L"текст", L"караоке", L"prod"};
    std::wstring r;
    size_t i = 0;
    while (i < s.size()) {
        wchar_t o = s[i];
        wchar_t c = o == L'(' ? L')' : o == L'[' ? L']' : 0;
        if (c) {
            size_t j = s.find(c, i + 1);
            if (j != std::wstring::npos) {
                std::wstring in = Lower(s.substr(i + 1, j - i - 1));
                bool bad = false;
                for (auto* k : junk)
                    if (in.find(k) != std::wstring::npos) { bad = true; break; }
                if (bad) { i = j + 1; continue; }
            }
        }
        r += o;
        i++;
    }
    // схлопываем двойные пробелы
    std::wstring t;
    for (wchar_t ch : r)
        if (!(ch == L' ' && !t.empty() && t.back() == L' ')) t += ch;
    return Trim(t);
}
static std::wstring CleanArtist(std::wstring a) {
    a = Trim(a);
    const wchar_t* suf[] = {L" - Topic", L"VEVO", L" Official"};
    for (auto* s : suf) {
        size_t n = wcslen(s);
        if (a.size() > n && Lower(a.substr(a.size() - n)) == Lower(s)) a = Trim(a.substr(0, a.size() - n));
    }
    return a;
}

// ---------------------------------------------------------------------------
// HTTP (WinHTTP, учитывает системный прокси)
// ---------------------------------------------------------------------------
static std::wstring UrlEnc(const std::wstring& w) {
    std::string s = ToU8(w);
    std::wstring o;
    static const char* hex = "0123456789ABCDEF";
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o += (wchar_t)c;
        else { o += L'%'; o += (wchar_t)hex[c >> 4]; o += (wchar_t)hex[c & 15]; }
    }
    return o;
}

struct Http {
    HINTERNET ses = nullptr, con = nullptr;
    ~Http() { Close(); }
    void Close() {
        if (con) { WinHttpCloseHandle(con); con = nullptr; }
        if (ses) { WinHttpCloseHandle(ses); ses = nullptr; }
    }
    bool Open() {
        if (con) return true;
        const wchar_t* ua = L"DynamicIsland-Windows/0.3 (derk)";
        ses = WinHttpOpen(ua, 4 /*WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY*/, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!ses) ses = WinHttpOpen(ua, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!ses) return false;
        WinHttpSetTimeouts(ses, 6000, 6000, 8000, 10000);
        con = WinHttpConnect(ses, L"lrclib.net", INTERNET_DEFAULT_HTTPS_PORT, 0);
        return con != nullptr;
    }
    // true = сервер ответил (status заполнен)
    bool Get(const std::wstring& path, DWORD& status, std::string& body) {
        status = 0;
        body.clear();
        if (!Open()) return false;
        HINTERNET r = WinHttpOpenRequest(con, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                         WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
        if (!r) return false;
        bool ok = false;
        if (WinHttpSendRequest(r, L"Accept: application/json\r\n", (DWORD)-1L, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
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
                if (body.size() > 4 * 1024 * 1024) break;
            }
            ok = status != 0;
        }
        WinHttpCloseHandle(r);
        if (!ok) Close();  // на случай сломанного соединения
        return ok;
    }
};

// ---------------------------------------------------------------------------
// Файлы кэша
// ---------------------------------------------------------------------------
static bool ReadFileAll(const std::wstring& path, std::string& out) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz;
    bool ok = GetFileSizeEx(f, &sz) && sz.QuadPart < 8 * 1024 * 1024;
    if (ok) {
        out.resize((size_t)sz.QuadPart);
        DWORD rd = 0;
        ok = out.empty() || (ReadFile(f, &out[0], (DWORD)out.size(), &rd, nullptr) && rd == out.size());
    }
    CloseHandle(f);
    return ok;
}
static void WriteFileAll(const std::wstring& path, const std::string& data) {
    std::wstring tmp = path + L".tmp";
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD wr = 0;
    WriteFile(f, data.data(), (DWORD)data.size(), &wr, nullptr);
    CloseHandle(f);
    MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
}
static double FileAgeDays(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a)) return -1;
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    ULARGE_INTEGER x, y;
    x.LowPart = a.ftLastWriteTime.dwLowDateTime; x.HighPart = a.ftLastWriteTime.dwHighDateTime;
    y.LowPart = now.dwLowDateTime; y.HighPart = now.dwHighDateTime;
    return (double)(y.QuadPart - x.QuadPart) / 1e7 / 86400.0;
}
static std::wstring HashHex(const std::wstring& s) {
    std::string u = ToU8(s);
    unsigned long long h = 1469598103934665603ULL;
    for (unsigned char c : u) { h ^= c; h *= 1099511628211ULL; }
    wchar_t b[20];
    swprintf(b, 20, L"%016llx", h);
    return b;
}

// ---------------------------------------------------------------------------
// Сервис
// ---------------------------------------------------------------------------
void LyricsService::Start(HWND notify, const std::wstring& cacheDir) {
    notify_ = notify;
    dir_ = cacheDir;
    SHCreateDirectoryExW(nullptr, dir_.c_str(), nullptr);
    th_ = std::thread([this] { Run(); });
}
void LyricsService::Stop() {
    {
        std::lock_guard<std::mutex> g(mx_);
        quit_ = true;
    }
    cv_.notify_all();
    if (th_.joinable()) th_.join();
}
UINT LyricsService::Request(const std::wstring& title, const std::wstring& artist, const std::wstring& album, int dur) {
    std::lock_guard<std::mutex> g(mx_);
    req_.id = ++nextId_;
    req_.title = title;
    req_.artist = artist;
    req_.album = album;
    req_.dur = dur;
    hasReq_ = true;
    cv_.notify_all();
    return req_.id;
}
bool LyricsService::Take(UINT id, Lyrics& out, int& state) {
    std::lock_guard<std::mutex> g(mx_);
    if (resId_ != id) return false;
    out = res_;
    state = resState_;
    return true;
}

void LyricsService::Run() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    for (;;) {
        Req r;
        {
            std::unique_lock<std::mutex> lk(mx_);
            cv_.wait(lk, [this] { return quit_ || hasReq_; });
            if (quit_) return;
            r = req_;
            hasReq_ = false;
        }
        Lyrics L;
        int st = Fetch(r, L);
        {
            std::lock_guard<std::mutex> g(mx_);
            if (quit_) return;
            if (r.id != nextId_) continue;  // уже устарело
            res_ = std::move(L);
            resState_ = st;
            resId_ = r.id;
        }
        PostMessageW(notify_, WM_APP_LYRICS, r.id, 0);
    }
}

// Выбираем лучший синхронизированный текст из результатов поиска
static bool PickBest(const JVal& arr, int dur, std::string& synced) {
    if (arr.t != JVal::Arr) return false;
    double best = 1e9;
    for (const JVal& it : arr.a) {
        const JVal* sl = it.Get("syncedLyrics");
        if (!sl || sl->t != JVal::Str || sl->s.size() < 10) continue;
        double d = 0;
        if (dur > 0) {
            const JVal* du = it.Get("duration");
            if (du && du->t == JVal::Num) d = std::fabs(du->n - dur);
            else d = 30;
            if (d > 6) continue;
        }
        if (d < best) { best = d; synced = sl->s; }
    }
    return !synced.empty();
}

int LyricsService::Fetch(const Req& r, Lyrics& out) {
    std::wstring title = StripJunkBrackets(r.title), artist = CleanArtist(r.artist), album = Trim(r.album);
    if (title.empty()) return LYR_NOTFOUND;

    std::wstring key = Lower(artist) + L"|" + Lower(title) + L"|" + std::to_wstring(r.dur);
    std::wstring base = dir_ + L"\\" + HashHex(key);
    std::wstring fLrc = base + L".lrc", fNone = base + L".none";

    std::string cached;
    if (ReadFileAll(fLrc, cached) && ParseLrc(FromU8(cached), out)) return LYR_FOUND;
    double age = FileAgeDays(fNone);
    if (age >= 0 && age < 3) return LYR_NOTFOUND;

    Http http;
    bool answered = false;
    std::string synced;
    DWORD status;
    std::string body;
    auto search = [&](const std::wstring& q) {
        if (!synced.empty()) return;
        if (!http.Get(q, status, body)) return;
        answered = true;
        JVal v;
        if (status == 200 && ParseJson(body, v)) PickBest(v, r.dur, synced);
    };

    // 1) точное совпадение (нужны альбом и длительность)
    if (!album.empty() && r.dur > 0 && !artist.empty()) {
        std::wstring q = L"/api/get?track_name=" + UrlEnc(title) + L"&artist_name=" + UrlEnc(artist) +
                         L"&album_name=" + UrlEnc(album) + L"&duration=" + std::to_wstring(r.dur);
        if (http.Get(q, status, body)) {
            answered = true;
            JVal v;
            if (status == 200 && ParseJson(body, v)) {
                const JVal* sl = v.Get("syncedLyrics");
                if (sl && sl->t == JVal::Str && sl->s.size() >= 10) synced = sl->s;
            }
        }
    }
    // 2) поиск по названию и артисту
    if (!artist.empty()) search(L"/api/search?track_name=" + UrlEnc(title) + L"&artist_name=" + UrlEnc(artist));
    // 3) YouTube-формат «Артист - Песня» в названии
    size_t dash = title.find(L" - ");
    if (dash == std::wstring::npos) dash = title.find(L" — ");
    if (dash != std::wstring::npos) {
        std::wstring a = Trim(title.substr(0, dash)), t = Trim(title.substr(dash + 3));
        if (!a.empty() && !t.empty()) search(L"/api/search?track_name=" + UrlEnc(t) + L"&artist_name=" + UrlEnc(a));
    }
    // 4) свободный запрос
    search(L"/api/search?q=" + UrlEnc(artist.empty() ? title : artist + L" " + title));

    if (!synced.empty()) {
        if (ParseLrc(FromU8(synced), out)) {
            WriteFileAll(fLrc, synced);
            return LYR_FOUND;
        }
    }
    if (answered) WriteFileAll(fNone, "");  // сервер ответил, но текста нет — не спрашиваем 3 дня
    return LYR_NOTFOUND;
}
