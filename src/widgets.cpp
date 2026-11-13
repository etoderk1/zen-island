// widgets.cpp — погода и календарь
#include "widgets.h"
#include "net.h"
#include <algorithm>
#include <map>
#include <set>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <climits>

// ---------------------------------------------------------------------------
// Погода
// ---------------------------------------------------------------------------
const wchar_t* WeatherText(int c) {
    switch (c) {
        case 0: return L"Ясно";
        case 1: return L"Почти ясно";
        case 2: return L"Переменная облачность";
        case 3: return L"Пасмурно";
        case 45: case 48: return L"Туман";
        case 51: case 53: case 55: return L"Морось";
        case 56: case 57: return L"Ледяная морось";
        case 61: return L"Небольшой дождь";
        case 63: return L"Дождь";
        case 65: return L"Сильный дождь";
        case 66: case 67: return L"Ледяной дождь";
        case 71: return L"Небольшой снег";
        case 73: return L"Снег";
        case 75: return L"Сильный снег";
        case 77: return L"Снежная крупа";
        case 80: case 81: return L"Ливень";
        case 82: return L"Сильный ливень";
        case 85: case 86: return L"Снегопад";
        case 95: return L"Гроза";
        case 96: case 99: return L"Гроза с градом";
    }
    return L"—";
}
int WeatherIcon(int c) {
    if (c == 0) return WX_CLEAR;
    if (c == 1 || c == 2) return WX_PARTLY;
    if (c == 3) return WX_CLOUD;
    if (c == 45 || c == 48) return WX_FOG;
    if (c >= 51 && c <= 57) return WX_DRIZZLE;
    if ((c >= 61 && c <= 67) || (c >= 80 && c <= 82)) return WX_RAIN;
    if ((c >= 71 && c <= 77) || c == 85 || c == 86) return WX_SNOW;
    if (c >= 95) return WX_THUNDER;
    return WX_CLOUD;
}

void WeatherService::Start(HWND notify, const std::wstring& city) {
    notify_ = notify;
    city_ = city;
    stopEv_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    wakeEv_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    th_ = std::thread([this] { Run(); });
}
void WeatherService::SetCity(const std::wstring& city) {
    {
        std::lock_guard<std::mutex> g(mx_);
        if (city == city_) return;
        city_ = city;
    }
    if (wakeEv_) SetEvent(wakeEv_);
}
void WeatherService::Refresh() { if (wakeEv_) SetEvent(wakeEv_); }
void WeatherService::Stop() {
    if (!stopEv_) return;
    SetEvent(stopEv_);
    if (th_.joinable()) th_.join();
    CloseHandle(stopEv_);
    CloseHandle(wakeEv_);
    stopEv_ = wakeEv_ = nullptr;
}

void WeatherService::Run() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    std::wstring resolvedFor = L"\x01";
    double lat = 0, lon = 0;
    std::wstring cityName;
    for (;;) {
        std::wstring city;
        {
            std::lock_guard<std::mutex> g(mx_);
            city = city_;
        }
        WeatherData* d = new WeatherData;
        bool okLoc = resolvedFor == city;
        DWORD st;
        std::string body;
        if (!okLoc) {
            net::JVal v;
            if (!city.empty()) {
                std::wstring u = L"https://geocoding-api.open-meteo.com/v1/search?count=1&language=ru&format=json&name=" + net::UrlEnc(city);
                if (net::HttpGet(u, st, body) && st == 200 && net::ParseJson(body, v)) {
                    const net::JVal* r = v.Get("results");
                    if (r && r->t == net::JVal::Arr && !r->a.empty()) {
                        lat = r->a[0].GetNum("latitude");
                        lon = r->a[0].GetNum("longitude");
                        cityName = net::FromU8(r->a[0].GetStr("name"));
                        okLoc = true;
                    } else {
                        d->error = L"Город не найден";
                    }
                }
            } else {
                // город не задан — примерно по IP (с VPN будет город сервера)
                if (net::HttpGet(L"http://ip-api.com/json/?fields=status,city,lat,lon&lang=ru", st, body) && st == 200 &&
                    net::ParseJson(body, v) && v.GetStr("status") == "success") {
                    lat = v.GetNum("lat");
                    lon = v.GetNum("lon");
                    cityName = net::FromU8(v.GetStr("city"));
                    okLoc = true;
                }
            }
            if (okLoc) resolvedFor = city;
        }
        if (okLoc) {
            wchar_t u[512];
            swprintf(u, 512,
                     L"https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
                     L"&current=temperature_2m,apparent_temperature,weather_code,is_day,wind_speed_10m,relative_humidity_2m,precipitation"
                     L"&hourly=temperature_2m,weather_code,precipitation_probability,is_day&forecast_hours=25"
                     L"&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_sum,sunrise,sunset"
                     L"&timezone=auto&forecast_days=7&wind_speed_unit=ms",
                     lat, lon);
            net::JVal v;
            if (net::HttpGet(u, st, body) && st == 200 && net::ParseJson(body, v)) {
                const net::JVal* c = v.Get("current");
                const net::JVal* dl = v.Get("daily");
                if (c && c->t == net::JVal::Obj) {
                    d->temp = c->GetNum("temperature_2m");
                    d->feels = c->GetNum("apparent_temperature", d->temp);
                    d->code = (int)c->GetNum("weather_code");
                    d->isDay = c->GetNum("is_day", 1) != 0;
                    d->wind = c->GetNum("wind_speed_10m");
                    d->humidity = c->GetNum("relative_humidity_2m");
                    d->precip = c->GetNum("precipitation");
                    d->ok = true;
                }
                const net::JVal* hr = v.Get("hourly");
                if (hr && hr->t == net::JVal::Obj) {
                    const net::JVal *tt = hr->Get("time"), *te = hr->Get("temperature_2m"), *wc = hr->Get("weather_code"),
                                    *pp = hr->Get("precipitation_probability"), *dy = hr->Get("is_day");
                    if (tt && te && tt->t == net::JVal::Arr && te->t == net::JVal::Arr) {
                        int n = (int)std::min<size_t>(24, std::min(tt->a.size(), te->a.size()));
                        for (int i = 0; i < n; i++) {
                            const std::string& ts = tt->a[i].s;  // 2026-11-12T13:00
                            d->hhour[i] = ts.size() >= 13 ? atoi(ts.substr(11, 2).c_str()) : 0;
                            d->htemp[i] = te->a[i].n;
                            if (wc && wc->t == net::JVal::Arr && i < (int)wc->a.size()) d->hcode[i] = (int)wc->a[i].n;
                            if (pp && pp->t == net::JVal::Arr && i < (int)pp->a.size()) d->hprob[i] = (int)pp->a[i].n;
                            d->hday[i] = !(dy && dy->t == net::JVal::Arr && i < (int)dy->a.size()) || dy->a[i].n != 0;
                        }
                        d->hours = n;
                    }
                }
                if (dl && dl->t == net::JVal::Obj) {
                    const net::JVal *mx = dl->Get("temperature_2m_max"), *mn = dl->Get("temperature_2m_min"), *wc = dl->Get("weather_code");
                    const net::JVal *ps = dl->Get("precipitation_sum"), *tm = dl->Get("time"), *sr = dl->Get("sunrise"), *ss = dl->Get("sunset");
                    for (int i = 0; i < 7; i++) {
                        if (!(mx && mx->t == net::JVal::Arr && i < (int)mx->a.size())) break;
                        d->tmax[i] = mx->a[i].n;
                        if (mn && mn->t == net::JVal::Arr && i < (int)mn->a.size()) d->tmin[i] = mn->a[i].n;
                        if (wc && wc->t == net::JVal::Arr && i < (int)wc->a.size()) d->dcode[i] = (int)wc->a[i].n;
                        if (ps && ps->t == net::JVal::Arr && i < (int)ps->a.size()) d->dprecip[i] = ps->a[i].n;
                        if (tm && tm->t == net::JVal::Arr && i < (int)tm->a.size() && tm->a[i].s.size() >= 10) {
                            const std::string& t = tm->a[i].s;
                            SYSTEMTIME st = {};
                            st.wYear = (WORD)atoi(t.substr(0, 4).c_str());
                            st.wMonth = (WORD)atoi(t.substr(5, 2).c_str());
                            st.wDay = (WORD)atoi(t.substr(8, 2).c_str());
                            FILETIME ft;
                            SystemTimeToFileTime(&st, &ft);
                            FileTimeToSystemTime(&ft, &st);
                            d->dweekday[i] = st.wDayOfWeek;
                        }
                        d->days = i + 1;
                    }
                    auto hhmm = [](const net::JVal* a) -> std::wstring {
                        if (!a || a->t != net::JVal::Arr || a->a.empty() || a->a[0].s.size() < 16) return L"";
                        return net::FromU8(a->a[0].s.substr(11, 5));
                    };
                    d->sunrise = hhmm(sr);
                    d->sunset = hhmm(ss);
                }
            }
            if (!d->ok && d->error.empty()) d->error = L"Нет связи с Open-Meteo";
        } else if (d->error.empty()) {
            d->error = L"Не удалось определить город";
        }
        d->city = cityName;
        if (!PostMessageW(notify_, WM_APP_WEATHER, 0, (LPARAM)d)) delete d;
        HANDLE w[2] = {stopEv_, wakeEv_};
        DWORD r = WaitForMultipleObjects(2, w, FALSE, okLoc ? 20 * 60 * 1000 : 3 * 60 * 1000);
        if (r == WAIT_OBJECT_0 || r == WAIT_FAILED) break;
    }
}

// ---------------------------------------------------------------------------
// Календарь: ICS
// ---------------------------------------------------------------------------
static long long FtOf(const SYSTEMTIME& st) {
    FILETIME ft;
    if (!SystemTimeToFileTime(&st, &ft)) return 0;
    return ((long long)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
}
static long long LocalToUtc(SYSTEMTIME local) {
    SYSTEMTIME utc;
    if (!TzSpecificLocalTimeToSystemTime(nullptr, &local, &utc)) return FtOf(local);
    return FtOf(utc);
}
// дни от 1970-01-01 (алгоритм Хиннанта)
static long long DaysFromCivil(int y, int m, int d) {
    y -= m <= 2;
    long long era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long long)doe - 719468;
}
static void CivilFromDays(long long z, int& y, int& m, int& d) {
    z += 719468;
    long long era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long long yy = (long long)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    d = (int)(doy - (153 * mp + 2) / 5 + 1);
    m = (int)(mp < 10 ? mp + 3 : mp - 9);
    y = (int)(yy + (m <= 2));
}
static int DaysInMonth(int y, int m) {
    static const int dm[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) return 29;
    return dm[(m - 1) % 12];
}

struct LTime { int y = 0, m = 0, d = 0, hh = 0, mi = 0, ss = 0; bool utc = false, dateOnly = false; };
static bool ParseLTime(const std::string& v, LTime& t) {
    if (v.size() < 8) return false;
    t.y = atoi(v.substr(0, 4).c_str());
    t.m = atoi(v.substr(4, 2).c_str());
    t.d = atoi(v.substr(6, 2).c_str());
    if (v.size() >= 15 && v[8] == 'T') {
        t.hh = atoi(v.substr(9, 2).c_str());
        t.mi = atoi(v.substr(11, 2).c_str());
        t.ss = atoi(v.substr(13, 2).c_str());
        t.utc = v.size() >= 16 && v[15] == 'Z';
    } else {
        t.dateOnly = true;
    }
    return t.y > 1900 && t.m >= 1 && t.m <= 12 && t.d >= 1 && t.d <= 31;
}
static long long ToFt(const LTime& t) {
    SYSTEMTIME s = {};
    s.wYear = (WORD)t.y; s.wMonth = (WORD)t.m; s.wDay = (WORD)t.d;
    s.wHour = (WORD)t.hh; s.wMinute = (WORD)t.mi; s.wSecond = (WORD)t.ss;
    return t.utc ? FtOf(s) : LocalToUtc(s);
}
static long long ParseDuration(const std::string& v) {  // P1DT2H30M
    long long secs = 0, num = 0;
    bool neg = false;
    for (char c : v) {
        if (c == '-') neg = true;
        else if (c >= '0' && c <= '9') num = num * 10 + (c - '0');
        else if (c == 'W') { secs += num * 7 * 86400; num = 0; }
        else if (c == 'D') { secs += num * 86400; num = 0; }
        else if (c == 'H') { secs += num * 3600; num = 0; }
        else if (c == 'M') { secs += num * 60; num = 0; }
        else if (c == 'S') { secs += num; num = 0; }
    }
    return (neg ? -secs : secs) * 10000000LL;
}
static std::wstring Unescape(const std::string& s) {
    std::string r;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            char n = s[++i];
            if (n == 'n' || n == 'N') r += ' ';
            else r += n;
        } else r += s[i];
    }
    return net::FromU8(r);
}

struct VEvent {
    std::map<std::string, std::string> props;   // имя -> значение (первое)
    std::map<std::string, std::string> params;  // имя -> параметры
    std::vector<std::string> exdates;
};

bool ParseIcs(const std::string& raw, long long from, long long to, std::vector<CalEvent>& out) {
    // разворачиваем перенесённые строки
    std::string s;
    s.reserve(raw.size());
    for (size_t i = 0; i < raw.size(); i++) {
        if (raw[i] == '\r') continue;
        if (raw[i] == '\n' && i + 1 < raw.size() && (raw[i + 1] == ' ' || raw[i + 1] == '\t')) { i++; continue; }
        s += raw[i];
    }
    std::vector<VEvent> evs;
    bool in = false;
    VEvent cur;
    size_t p = 0;
    bool sawCalendar = false;
    while (p < s.size()) {
        size_t e = s.find('\n', p);
        std::string line = s.substr(p, e == std::string::npos ? std::string::npos : e - p);
        p = e == std::string::npos ? s.size() : e + 1;
        if (line == "BEGIN:VCALENDAR") sawCalendar = true;
        if (line == "BEGIN:VEVENT") { in = true; cur = VEvent{}; continue; }
        if (line == "END:VEVENT") { if (in) evs.push_back(cur); in = false; continue; }
        if (!in) continue;
        size_t colon = std::string::npos;
        bool q = false;
        for (size_t i = 0; i < line.size(); i++) {  // двоеточие вне кавычек
            if (line[i] == '"') q = !q;
            if (line[i] == ':' && !q) { colon = i; break; }
        }
        if (colon == std::string::npos) continue;
        std::string left = line.substr(0, colon), val = line.substr(colon + 1);
        size_t semi = left.find(';');
        std::string name = left.substr(0, semi), prm = semi == std::string::npos ? "" : left.substr(semi + 1);
        for (auto& c : name) c = (char)toupper((unsigned char)c);
        if (name == "EXDATE") {
            size_t a = 0;
            while (a <= val.size()) {
                size_t b = val.find(',', a);
                cur.exdates.push_back(val.substr(a, b == std::string::npos ? std::string::npos : b - a));
                if (b == std::string::npos) break;
                a = b + 1;
            }
            continue;
        }
        if (!cur.props.count(name)) { cur.props[name] = val; cur.params[name] = prm; }
    }
    if (!sawCalendar) return false;

    // переопределённые экземпляры повторяющихся событий (RECURRENCE-ID)
    std::set<std::pair<std::string, long long>> overridden;
    for (auto& ev : evs) {
        auto r = ev.props.find("RECURRENCE-ID");
        if (r == ev.props.end()) continue;
        LTime t;
        if (ParseLTime(r->second, t)) overridden.insert({ev.props["UID"], ToFt(t)});
    }

    for (auto& ev : evs) {
        if (ev.props["STATUS"] == "CANCELLED") continue;
        LTime ts;
        if (!ParseLTime(ev.props["DTSTART"], ts)) continue;
        long long start = ToFt(ts), dur = 0;
        LTime te;
        if (ev.props.count("DTEND") && ParseLTime(ev.props["DTEND"], te)) dur = ToFt(te) - start;
        else if (ev.props.count("DURATION")) dur = ParseDuration(ev.props["DURATION"]);
        else if (ts.dateOnly) dur = 86400LL * 10000000LL;
        if (dur < 0) dur = 0;
        CalEvent base;
        base.allDay = ts.dateOnly;
        base.title = Unescape(ev.props["SUMMARY"]);
        base.location = Unescape(ev.props["LOCATION"]);
        if (base.title.empty()) base.title = L"(без названия)";
        std::string uid = ev.props["UID"];
        std::set<long long> ex;
        for (auto& x : ev.exdates) { LTime t; if (ParseLTime(x, t)) { if (t.dateOnly) { t.hh = ts.hh; t.mi = ts.mi; t.ss = ts.ss; t.utc = ts.utc; } ex.insert(ToFt(t)); } }

        auto emit = [&](long long st) {
            if (st < to && st + std::max(dur, 1LL) > from && !ex.count(st) &&
                (ev.props.count("RECURRENCE-ID") || !overridden.count({uid, st}))) {
                CalEvent c = base;
                c.start = st;
                c.end = st + dur;
                out.push_back(c);
            }
        };
        auto rr = ev.props.find("RRULE");
        if (rr == ev.props.end() || ev.props.count("RECURRENCE-ID")) { emit(start); continue; }

        // RRULE: FREQ, INTERVAL, COUNT, UNTIL, BYDAY (для WEEKLY)
        std::map<std::string, std::string> rule;
        {
            std::string r = rr->second;
            size_t a = 0;
            while (a < r.size()) {
                size_t b = r.find(';', a);
                std::string kv = r.substr(a, b == std::string::npos ? std::string::npos : b - a);
                size_t eq = kv.find('=');
                if (eq != std::string::npos) rule[kv.substr(0, eq)] = kv.substr(eq + 1);
                if (b == std::string::npos) break;
                a = b + 1;
            }
        }
        std::string freq = rule["FREQ"];
        int interval = std::max(1, atoi(rule["INTERVAL"].c_str()));
        int count = rule.count("COUNT") ? atoi(rule["COUNT"].c_str()) : -1;
        long long until = LLONG_MAX;
        if (rule.count("UNTIL")) { LTime u; if (ParseLTime(rule["UNTIL"], u)) { if (u.dateOnly) { u.hh = 23; u.mi = 59; u.ss = 59; } until = ToFt(u); } }
        long long day0 = DaysFromCivil(ts.y, ts.m, ts.d);
        int n = 0;
        auto occur = [&](int y, int m, int d) -> bool {  // false — остановиться
            LTime t = ts;
            t.y = y; t.m = m; t.d = d;
            long long st = ToFt(t);
            if (st > until || st >= to) return false;
            if (count >= 0 && n >= count) return false;
            n++;
            emit(st);
            return true;
        };
        int guard = 0;
        if (freq == "DAILY") {
            for (long long k = 0; guard++ < 5000; k += interval) {
                int y, m, d;
                CivilFromDays(day0 + k, y, m, d);
                if (!occur(y, m, d)) break;
            }
        } else if (freq == "WEEKLY") {
            static const char* names[7] = {"MO", "TU", "WE", "TH", "FR", "SA", "SU"};
            std::vector<int> days;
            std::string by = rule["BYDAY"];
            for (int i = 0; i < 7; i++) if (by.find(names[i]) != std::string::npos) days.push_back(i);
            int wd0 = (int)((day0 % 7 + 7 + 3) % 7);  // 0 = понедельник
            if (days.empty()) days.push_back(wd0);
            long long weekStart = day0 - wd0;
            bool stop = false;
            for (long long w = 0; !stop && guard++ < 3000; w += interval) {
                for (int di : days) {
                    long long dd = weekStart + w * 7 + di;
                    if (dd < day0) continue;
                    int y, m, d;
                    CivilFromDays(dd, y, m, d);
                    if (!occur(y, m, d)) { stop = true; break; }
                }
            }
        } else if (freq == "MONTHLY") {
            for (int k = 0; guard++ < 1200; k += interval) {
                int y = ts.y + (ts.m - 1 + k) / 12, m = (ts.m - 1 + k) % 12 + 1;
                if (ts.d > DaysInMonth(y, m)) continue;
                if (!occur(y, m, ts.d)) break;
            }
        } else if (freq == "YEARLY") {
            for (int k = 0; guard++ < 200; k += interval) {
                if (ts.d > DaysInMonth(ts.y + k, ts.m)) continue;
                if (!occur(ts.y + k, ts.m, ts.d)) break;
            }
        } else {
            emit(start);
        }
    }
    std::sort(out.begin(), out.end(), [](const CalEvent& a, const CalEvent& b) { return a.start < b.start; });
    return true;
}

static long long NowFt() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return ((long long)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
}

void CalendarService::Start(HWND notify, const std::wstring& url) {
    notify_ = notify;
    url_ = url;
    stopEv_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    wakeEv_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    th_ = std::thread([this] { Run(); });
}
void CalendarService::SetUrl(const std::wstring& url) {
    {
        std::lock_guard<std::mutex> g(mx_);
        if (url == url_) return;
        url_ = url;
    }
    if (wakeEv_) SetEvent(wakeEv_);
}
void CalendarService::Stop() {
    if (!stopEv_) return;
    SetEvent(stopEv_);
    if (th_.joinable()) th_.join();
    CloseHandle(stopEv_);
    CloseHandle(wakeEv_);
    stopEv_ = wakeEv_ = nullptr;
}
void CalendarService::Run() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    for (;;) {
        std::wstring url;
        {
            std::lock_guard<std::mutex> g(mx_);
            url = url_;
        }
        CalData* d = new CalData;
        bool fetched = false;
        if (!url.empty()) {
            if (_wcsnicmp(url.c_str(), L"webcal://", 9) == 0) url = L"https://" + url.substr(9);
            DWORD st;
            std::string body;
            if (net::HttpGet(url, st, body, 15000) && st == 200) {
                long long now = NowFt();
                std::vector<CalEvent> all;
                if (ParseIcs(body, now - 86400LL * 10000000LL, now + 14 * 86400LL * 10000000LL, all)) {
                    for (auto& e : all) if (e.end > now || (e.end == e.start && e.start > now - 3600LL * 10000000LL)) d->events.push_back(e);
                    if (d->events.size() > 40) d->events.resize(40);
                    d->ok = fetched = true;
                } else d->error = L"Это не похоже на ICS-календарь";
            } else d->error = st ? L"Календарь недоступен (код " + std::to_wstring(st) + L")" : L"Нет связи с календарём";
        }
        if (!PostMessageW(notify_, WM_APP_CAL, 0, (LPARAM)d)) delete d;
        HANDLE w[2] = {stopEv_, wakeEv_};
        DWORD r = WaitForMultipleObjects(2, w, FALSE, url.empty() ? INFINITE : (fetched ? 15 * 60 * 1000 : 3 * 60 * 1000));
        if (r == WAIT_OBJECT_0 || r == WAIT_FAILED) break;
    }
}
