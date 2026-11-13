// reminders.cpp — разбор русской фразы в напоминание
#include "reminders.h"
#include <regex>
#include <cwctype>
#include <cstdio>

static long long FtOf(const SYSTEMTIME& st) {
    FILETIME ft;
    SystemTimeToFileTime(&st, &ft);
    return ((long long)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
}
static SYSTEMTIME LocalOf(long long utc) {
    FILETIME ft, lft;
    ft.dwLowDateTime = (DWORD)utc;
    ft.dwHighDateTime = (DWORD)(utc >> 32);
    SYSTEMTIME st;
    FileTimeToLocalFileTime(&ft, &lft);
    FileTimeToSystemTime(&lft, &st);
    return st;
}
static long long UtcOfLocal(SYSTEMTIME local) {
    SYSTEMTIME utc;
    if (!TzSpecificLocalTimeToSystemTime(nullptr, &local, &utc)) utc = local;
    return FtOf(utc);
}
static const long long MIN = 60LL * 10000000LL, DAY = 1440 * MIN;

// локальная дата + days
static SYSTEMTIME AddDays(SYSTEMTIME st, int days) {
    st.wHour = 12; st.wMinute = st.wSecond = st.wMilliseconds = 0;
    long long f = FtOf(st) + days * DAY;
    FILETIME ft;
    ft.dwLowDateTime = (DWORD)f;
    ft.dwHighDateTime = (DWORD)(f >> 32);
    SYSTEMTIME r;
    FileTimeToSystemTime(&ft, &r);
    return r;
}

bool ParseReminder(const std::wstring& phrase, long long now, Reminder& out, std::wstring& err) {
    std::wstring orig = phrase, s = phrase;
    if (!s.empty()) CharLowerBuffW(&s[0], (DWORD)s.size());
    for (auto& c : s) if (c == L'ё') c = L'е';
    SYSTEMTIME lnow = LocalOf(now);
    std::wsmatch m;
    long long due = 0;
    bool daily = false;
    size_t cutPos = std::wstring::npos, cutLen = 0;
    auto cut = [&](const std::wsmatch& mm) { cutPos = (size_t)mm.position(0); cutLen = (size_t)mm.length(0); };

    static const std::wregex reIn(L"через\\s+(\\d+|полчаса|час|минуту|пару минут)\\s*(минут[уы]?|мин\\.?|м|час(?:а|ов)?|ч)?(?=\\s|$|[,.!])");
    static const std::wregex reDaily(L"(каждый\\s+день|ежедневно)\\s*(?:в\\s+)?(\\d{1,2})(?:[:.](\\d{2}))?");
    static const std::wregex reAt(L"(?:(сегодня|завтра|послезавтра)\\s*)?(?:в|к)\\s+(\\d{1,2})(?:[:.](\\d{2}))?\\s*(утра|вечера|дня|ночи)?(?:\\s*(сегодня|завтра|послезавтра))?");
    static const std::wregex reWd(L"(?:в|во)\\s+(понедельник|вторник|среду|четверг|пятницу|субботу|воскресенье)(?:\\s+в\\s+(\\d{1,2})(?:[:.](\\d{2}))?)?");
    static const std::wregex reDay(L"(завтра|послезавтра)");

    auto hm = [](const std::wssub_match& h, const std::wssub_match& mi, const std::wssub_match* part, int& H, int& M) {
        H = _wtoi(h.str().c_str());
        M = mi.matched ? _wtoi(mi.str().c_str()) : 0;
        if (part && part->matched) {
            std::wstring p = part->str();
            if ((p == L"вечера" || p == L"дня") && H < 12) H += 12;
            if (p == L"ночи" && H == 12) H = 0;
        }
        return H >= 0 && H < 24 && M >= 0 && M < 60;
    };

    if (std::regex_search(s, m, reIn)) {
        std::wstring n = m[1].str(), unit = m[2].matched ? m[2].str() : L"";
        long long mins;
        if (n == L"полчаса") mins = 30;
        else if (n == L"час") mins = 60;
        else if (n == L"минуту") mins = 1;
        else if (n == L"пару минут") mins = 2;
        else {
            mins = _wtoi(n.c_str());
            if (!unit.empty() && unit[0] == L'ч') mins *= 60;
        }
        due = now + mins * MIN;
        cut(m);
    } else if (std::regex_search(s, m, reDaily)) {
        int H, M;
        if (!hm(m[2], m[3], nullptr, H, M)) { err = L"Не понял время"; return false; }
        SYSTEMTIME t = lnow;
        t.wHour = (WORD)H; t.wMinute = (WORD)M; t.wSecond = t.wMilliseconds = 0;
        due = UtcOfLocal(t);
        if (due <= now) due = UtcOfLocal([&] { SYSTEMTIME x = AddDays(lnow, 1); x.wHour = (WORD)H; x.wMinute = (WORD)M; x.wSecond = 0; return x; }());
        daily = true;
        cut(m);
    } else if (std::regex_search(s, m, reWd)) {
        static const wchar_t* days[] = {L"воскресенье", L"понедельник", L"вторник", L"среду", L"четверг", L"пятницу", L"субботу"};
        int wd = 0;
        for (int i = 0; i < 7; i++) if (m[1].str() == days[i]) wd = i;
        int H = 9, M = 0;
        if (m[2].matched && !hm(m[2], m[3], nullptr, H, M)) { err = L"Не понял время"; return false; }
        int add = (wd - lnow.wDayOfWeek + 7) % 7;
        if (add == 0) add = 7;
        SYSTEMTIME t = AddDays(lnow, add);
        t.wHour = (WORD)H; t.wMinute = (WORD)M; t.wSecond = 0;
        due = UtcOfLocal(t);
        cut(m);
    } else if (std::regex_search(s, m, reAt)) {
        int H, M;
        if (!hm(m[2], m[3], &m[4], H, M)) { err = L"Не понял время"; return false; }
        std::wstring day = m[1].matched ? m[1].str() : (m[5].matched ? m[5].str() : L"");
        size_t pos = (size_t)m.position(0), len = (size_t)m.length(0);
        std::wsmatch d2;
        if (day.empty() && std::regex_search(s, d2, reDay)) day = d2[1].str();  // «завтра … в 9»
        int add = day == L"завтра" ? 1 : day == L"послезавтра" ? 2 : 0;
        SYSTEMTIME t = AddDays(lnow, add);
        t.wHour = (WORD)H; t.wMinute = (WORD)M; t.wSecond = 0;
        due = UtcOfLocal(t);
        if (due <= now && day.empty()) {  // «в 9», а 9 уже прошло — значит завтра
            t = AddDays(lnow, 1);
            t.wHour = (WORD)H; t.wMinute = (WORD)M; t.wSecond = 0;
            due = UtcOfLocal(t);
        }
        cutPos = pos;
        cutLen = len;
        if (!m[1].matched && !m[5].matched && !d2.empty()) {
            // вырезаем и отдельно стоящее «завтра»
            std::wstring tmp = orig;
            tmp.erase(pos, len);
            size_t dp = d2.position(0) > (long)pos ? d2.position(0) - len : d2.position(0);
            tmp.erase(dp, d2.length(0));
            orig = tmp;
            cutPos = std::wstring::npos;
        }
    } else {
        err = L"Не понял, когда напомнить. Например: «завтра в 9», «через 20 минут», «каждый день в 23:00»";
        return false;
    }
    if (due <= now) { err = L"Это время уже прошло"; return false; }

    std::wstring text = orig;
    if (cutPos != std::wstring::npos && cutPos + cutLen <= text.size()) text.erase(cutPos, cutLen);
    // «напомни мне …»
    std::wstring low = text;
    if (!low.empty()) CharLowerBuffW(&low[0], (DWORD)low.size());
    static const wchar_t* pre[] = {L"напомни мне ", L"напомнить мне ", L"напомни ", L"напомнить ", L"напоминание "};
    size_t a = low.find_first_not_of(L" ,");
    if (a != std::wstring::npos)
        for (auto* p : pre)
            if (low.compare(a, wcslen(p), p) == 0) { text.erase(0, a + wcslen(p)); break; }
    // чистим пробелы и знаки по краям
    std::wstring r;
    for (wchar_t c : text) if (!(c == L' ' && (r.empty() || r.back() == L' '))) r += c;
    while (!r.empty() && (iswspace(r.back()) || r.back() == L',' || r.back() == L'.')) r.pop_back();
    while (!r.empty() && (iswspace(r.front()) || r.front() == L',')) r.erase(r.begin());
    if (r.empty()) r = L"Напоминание";
    CharUpperBuffW(&r[0], 1);
    out.text = r;
    out.due = due;
    out.daily = daily;
    out.id = now;
    return true;
}

std::wstring ReminderWhen(long long due, long long now) {
    SYSTEMTIME d = LocalOf(due), n = LocalOf(now);
    wchar_t b[64];
    long long diff = due - now;
    if (diff >= 0 && diff < 60 * MIN) {
        swprintf(b, 64, L"через %d мин", (int)((diff + MIN - 1) / MIN));
        return b;
    }
    auto dayIdx = [](SYSTEMTIME t) { t.wHour = t.wMinute = t.wSecond = t.wMilliseconds = 0; return FtOf(t) / DAY; };
    long long dd = dayIdx(d) - dayIdx(n);
    if (dd == 0) swprintf(b, 64, L"сегодня %d:%02d", d.wHour, d.wMinute);
    else if (dd == 1) swprintf(b, 64, L"завтра %d:%02d", d.wHour, d.wMinute);
    else swprintf(b, 64, L"%02d.%02d %d:%02d", d.wDay, d.wMonth, d.wHour, d.wMinute);
    return b;
}

void SaveReminders(const std::wstring& path, const std::vector<Reminder>& v) {
    std::wstring all;
    for (auto& r : v) {
        std::wstring t = r.text;
        for (auto& c : t) if (c == L'\n' || c == L'\r' || c == L'|') c = L' ';
        all += std::to_wstring(r.id) + L"|" + std::to_wstring(r.due) + L"|" + (r.daily ? L"1" : L"0") + L"|" + t + L"\n";
    }
    int n = WideCharToMultiByte(CP_UTF8, 0, all.c_str(), (int)all.size(), nullptr, 0, nullptr, nullptr);
    std::string u(n, '\0');
    if (n) WideCharToMultiByte(CP_UTF8, 0, all.c_str(), (int)all.size(), &u[0], n, nullptr, nullptr);
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD wr;
    WriteFile(f, u.data(), (DWORD)u.size(), &wr, nullptr);
    CloseHandle(f);
}
std::vector<Reminder> LoadReminders(const std::wstring& path) {
    std::vector<Reminder> v;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return v;
    LARGE_INTEGER sz;
    std::string u;
    if (GetFileSizeEx(f, &sz) && sz.QuadPart < 1024 * 1024) {
        u.resize((size_t)sz.QuadPart);
        DWORD rd;
        if (!u.empty()) ReadFile(f, &u[0], (DWORD)u.size(), &rd, nullptr);
    }
    CloseHandle(f);
    int n = MultiByteToWideChar(CP_UTF8, 0, u.data(), (int)u.size(), nullptr, 0);
    std::wstring all(n, L'\0');
    if (n) MultiByteToWideChar(CP_UTF8, 0, u.data(), (int)u.size(), &all[0], n);
    size_t a = 0;
    while (a < all.size()) {
        size_t e = all.find(L'\n', a);
        std::wstring line = all.substr(a, e == std::wstring::npos ? std::wstring::npos : e - a);
        size_t p1 = line.find(L'|'), p2 = line.find(L'|', p1 + 1), p3 = line.find(L'|', p2 + 1);
        if (p1 != std::wstring::npos && p2 != std::wstring::npos && p3 != std::wstring::npos) {
            Reminder r;
            r.id = _wtoi64(line.substr(0, p1).c_str());
            r.due = _wtoi64(line.substr(p1 + 1, p2 - p1 - 1).c_str());
            r.daily = line[p2 + 1] == L'1';
            r.text = line.substr(p3 + 1);
            if (r.due > 0) v.push_back(r);
        }
        if (e == std::wstring::npos) break;
        a = e + 1;
    }
    return v;
}
