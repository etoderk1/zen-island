// cmdlogic.cpp — калькулятор, единицы, валюты и нечёткий поиск для командной строки
#include "cmdlogic.h"
#include <windows.h>
#include <cmath>
#include <cwchar>
#include <cwctype>
#include <vector>

std::wstring LowerW(std::wstring s) {
    if (!s.empty()) CharLowerBuffW(&s[0], (DWORD)s.size());
    return s;
}

// ---------------------------------------------------------------- калькулятор
namespace {
const double kPi = 3.14159265358979323846;
struct Val { double v; bool pct; };
struct Parser {
    std::wstring s;
    size_t p = 0;
    bool ok = true, sawOp = false;
    void ws() { while (p < s.size() && (s[p] == L' ' || s[p] == L'\t')) p++; }
    bool eat(wchar_t c) { ws(); if (p < s.size() && s[p] == c) { p++; return true; } return false; }
    bool isIdentCh(wchar_t c) { return iswalpha(c) || (c >= 0x400 && c <= 0x4FF); }
    Val expr() {
        Val a = term();
        for (;;) {
            ws();
            if (p >= s.size()) break;
            wchar_t c = s[p];
            if (c != L'+' && c != L'-' && c != 0x2212) break;
            p++;
            sawOp = true;
            Val b = term();
            double sign = c == L'+' ? 1 : -1;
            if (b.pct && !a.pct) a.v = a.v * (1 + sign * b.v);  // 3400 + 15% = 3910
            else a.v = a.v + sign * b.v;
            a.pct = false;
        }
        return a;
    }
    Val term() {
        Val a = unary();
        for (;;) {
            ws();
            if (p >= s.size()) break;
            wchar_t c = s[p];
            if (c == L'*' || c == 0xD7 || c == 0x22C5 || c == 0xB7) { p++; sawOp = true; a.v *= unary().v; }
            else if (c == L'/' || c == 0xF7 || c == L':') { p++; sawOp = true; double b = unary().v; if (b == 0) ok = false; a.v = b ? a.v / b : 0; }
            else if (c == L'(' || isIdentCh(c)) { sawOp = true; a.v *= unary().v; }  // 2pi, 3(4+1)
            else break;
            a.pct = false;
        }
        return a;
    }
    Val unary() {
        ws();
        if (p < s.size() && (s[p] == L'-' || s[p] == 0x2212)) { p++; Val v = unary(); v.v = -v.v; return v; }
        if (p < s.size() && s[p] == L'+') { p++; return unary(); }
        return power();
    }
    Val power() {
        Val a = postfix();
        ws();
        if (p < s.size() && (s[p] == L'^' || (s[p] == L'*' && p + 1 < s.size() && s[p + 1] == L'*'))) {
            p += s[p] == L'^' ? 1 : 2;
            sawOp = true;
            Val b = unary();
            a.v = std::pow(a.v, b.v);
            a.pct = false;
        }
        return a;
    }
    Val postfix() {
        Val a = primary();
        for (;;) {
            ws();
            if (p < s.size() && s[p] == L'%') { p++; sawOp = true; a.v /= 100; a.pct = true; continue; }
            if (p < s.size() && s[p] == L'!') {
                p++;
                sawOp = true;
                if (a.v < 0 || a.v > 170 || a.v != std::floor(a.v)) { ok = false; return a; }
                double f = 1;
                for (int i = 2; i <= (int)a.v; i++) f *= i;
                a.v = f;
                continue;
            }
            if (p < s.size() && s[p] == 0xB2) { p++; sawOp = true; a.v *= a.v; continue; }  // ²
            break;
        }
        return a;
    }
    Val primary() {
        ws();
        if (p >= s.size()) { ok = false; return {0, false}; }
        wchar_t c = s[p];
        if (c == L'(') {
            p++;
            Val v = expr();
            if (!eat(L')')) ok = false;
            v.pct = false;
            return v;
        }
        if (iswdigit(c) || ((c == L'.' || c == L',') && p + 1 < s.size() && iswdigit(s[p + 1]))) {
            std::wstring num;
            bool dot = false;
            while (p < s.size()) {
                wchar_t d = s[p];
                if (iswdigit(d)) num += d;
                else if ((d == L'.' || d == L',') && !dot && p + 1 < s.size() && iswdigit(s[p + 1])) { num += L'.'; dot = true; }
                else if ((d == L' ' || d == 0xA0) && p + 3 < s.size() && iswdigit(s[p + 1]) && iswdigit(s[p + 2]) && iswdigit(s[p + 3]) &&
                         (p + 4 >= s.size() || !iswdigit(s[p + 4])) && !dot) { /* 1 000 */ }
                else break;
                p++;
            }
            return {wcstod(num.c_str(), nullptr), false};
        }
        if (isIdentCh(c)) {
            size_t b = p;
            while (p < s.size() && isIdentCh(s[p])) p++;
            std::wstring id = LowerW(s.substr(b, p - b));
            if (id == L"pi" || id == L"пи" || id == L"π") return {kPi, false};
            if (id == L"e") return {std::exp(1.0), false};
            struct F { const wchar_t* n; int k; } fs[] = {
                {L"sqrt", 0}, {L"корень", 0}, {L"кор", 0}, {L"sin", 1}, {L"cos", 2}, {L"tan", 3}, {L"tg", 3}, {L"ln", 4},
                {L"log", 5}, {L"lg", 5}, {L"abs", 6}, {L"round", 7}, {L"floor", 8}, {L"ceil", 9}, {L"cbrt", 10}};
            for (auto& f : fs) {
                if (id != f.n) continue;
                ws();
                Val a = (p < s.size() && s[p] == L'(') ? primary() : unary();
                sawOp = true;
                double x = a.v, r = 0;
                switch (f.k) {
                    case 0: if (x < 0) ok = false; r = std::sqrt(std::fabs(x)); break;
                    case 1: r = std::sin(x * kPi / 180); break;
                    case 2: r = std::cos(x * kPi / 180); break;
                    case 3: r = std::tan(x * kPi / 180); break;
                    case 4: if (x <= 0) ok = false; r = std::log(std::fabs(x) + 1e-300); break;
                    case 5: if (x <= 0) ok = false; r = std::log10(std::fabs(x) + 1e-300); break;
                    case 6: r = std::fabs(x); break;
                    case 7: r = std::round(x); break;
                    case 8: r = std::floor(x); break;
                    case 9: r = std::ceil(x); break;
                    case 10: r = std::cbrt(x); break;
                }
                if (std::fabs(r) < 1e-12) r = 0;  // sin(180) = 0, а не 1e-16
                return {r, false};
            }
            ok = false;
            return {0, false};
        }
        if (c == 0x221A) { p++; sawOp = true; Val a = unary(); if (a.v < 0) ok = false; return {std::sqrt(std::fabs(a.v)), false}; }  // √
        ok = false;
        return {0, false};
    }
};
std::wstring ReplaceAll(std::wstring s, const std::wstring& a, const std::wstring& b) {
    size_t p = 0;
    while ((p = s.find(a, p)) != std::wstring::npos) { s.replace(p, a.size(), b); p += b.size(); }
    return s;
}
}  // namespace

bool CalcEval(const std::wstring& expr0, double& out) {
    std::wstring e = LowerW(expr0);
    while (!e.empty() && (e.back() == L' ' || e.back() == L'=')) e.pop_back();
    if (e.empty()) return false;
    bool digit = false;
    for (wchar_t c : e) if (iswdigit(c)) { digit = true; break; }
    if (!digit && e.find(L"pi") == std::wstring::npos && e.find(L"пи") == std::wstring::npos) return false;
    // «15% от 3400», «15 процентов от 3400»
    e = ReplaceAll(e, L"процентов", L"%");
    e = ReplaceAll(e, L"процента", L"%");
    e = ReplaceAll(e, L"% от ", L"%*");
    e = ReplaceAll(e, L"% of ", L"%*");
    e = ReplaceAll(e, L"%от", L"%*");
    Parser ps;
    ps.s = e;
    Val v = ps.expr();
    ps.ws();
    if (!ps.ok || ps.p != e.size() || !ps.sawOp) return false;
    if (!std::isfinite(v.v)) return false;
    out = v.v;
    return true;
}

// ---------------------------------------------------------------- форматирование
static std::wstring FmtImpl(double v, bool group) {
    if (!std::isfinite(v)) return L"∞";
    double a = std::fabs(v);
    wchar_t b[64];
    if (a != 0 && (a >= 1e15 || a < 1e-6)) {
        swprintf(b, 64, L"%.6g", v);
        std::wstring s = b;
        for (auto& c : s) if (c == L'.') c = L',';
        return s;
    }
    int intDigits = a < 1 ? 1 : (int)std::floor(std::log10(a)) + 1;
    int dec = std::max(0, std::min(6, 8 - intDigits));
    swprintf(b, 64, L"%.*f", dec, v);
    std::wstring s = b;
    size_t dot = s.find(L'.');
    if (dot != std::wstring::npos) {
        while (!s.empty() && s.back() == L'0') s.pop_back();
        if (!s.empty() && s.back() == L'.') s.pop_back();
    }
    if (s == L"-0") s = L"0";
    dot = s.find(L'.');
    std::wstring ip = dot == std::wstring::npos ? s : s.substr(0, dot), fp = dot == std::wstring::npos ? L"" : s.substr(dot + 1);
    bool neg = !ip.empty() && ip[0] == L'-';
    if (neg) ip.erase(0, 1);
    if (group && ip.size() > 4) {
        std::wstring g;
        int n = 0;
        for (int i = (int)ip.size() - 1; i >= 0; i--) {
            g.insert(g.begin(), ip[i]);
            if (++n % 3 == 0 && i > 0) g.insert(g.begin(), (wchar_t)0x202F);
        }
        ip = g;
    }
    return (neg ? L"-" : L"") + ip + (fp.empty() ? L"" : L"," + fp);
}
std::wstring FmtNum(double v) { return FmtImpl(v, true); }
std::wstring FmtNumPlain(double v) { return FmtImpl(v, false); }

// ---------------------------------------------------------------- единицы
namespace {
enum Cat { C_LEN = 1, C_MASS, C_TEMP, C_VOL, C_SPEED, C_DATA, C_TIME, C_AREA, C_CUR };
struct Unit { const wchar_t* names; int cat; double f; const char* code; const wchar_t* show; };
const Unit kUnits[] = {
    {L"км|километр|километра|километров|km", C_LEN, 1000, nullptr, L"км"},
    {L"м|метр|метра|метров|m|meter|meters", C_LEN, 1, nullptr, L"м"},
    {L"см|сантиметр|сантиметра|сантиметров|cm", C_LEN, 0.01, nullptr, L"см"},
    {L"мм|миллиметр|миллиметра|миллиметров|mm", C_LEN, 0.001, nullptr, L"мм"},
    {L"миля|мили|миль|mi|mile|miles", C_LEN, 1609.344, nullptr, L"миль"},
    {L"ярд|ярда|ярдов|yd|yard|yards", C_LEN, 0.9144, nullptr, L"ярд."},
    {L"фут|фута|футов|ft|foot|feet", C_LEN, 0.3048, nullptr, L"фут."},
    {L"дюйм|дюйма|дюймов|in|inch|inches|\"", C_LEN, 0.0254, nullptr, L"дюйм."},
    {L"морская миля|морских миль|nmi", C_LEN, 1852, nullptr, L"мор. миль"},
    {L"т|тонна|тонны|тонн|t|ton|tons", C_MASS, 1000, nullptr, L"т"},
    {L"кг|килограмм|килограмма|килограммов|kg", C_MASS, 1, nullptr, L"кг"},
    {L"г|гр|грамм|грамма|граммов|g", C_MASS, 0.001, nullptr, L"г"},
    {L"мг|миллиграмм|mg", C_MASS, 1e-6, nullptr, L"мг"},
    {L"фунт|фунта|фунтов|lb|lbs|pound|pounds", C_MASS, 0.45359237, nullptr, L"фунт."},
    {L"унция|унции|унций|oz|ounce|ounces", C_MASS, 0.028349523125, nullptr, L"унц."},
    {L"c|°c|°с|цельсий|цельсия|celsius|градусов цельсия", C_TEMP, 0, nullptr, L"°C"},
    {L"f|°f|фаренгейт|фаренгейта|fahrenheit", C_TEMP, 1, nullptr, L"°F"},
    {L"k|кельвин|кельвина|кельвинов|kelvin", C_TEMP, 2, nullptr, L"K"},
    {L"л|литр|литра|литров|l|liter|liters|litre", C_VOL, 1, nullptr, L"л"},
    {L"мл|миллилитр|миллилитра|миллилитров|ml", C_VOL, 0.001, nullptr, L"мл"},
    {L"галлон|галлона|галлонов|gal|gallon|gallons", C_VOL, 3.785411784, nullptr, L"галл."},
    {L"м3|м³|кубометр|кубометра|кубометров|m3", C_VOL, 1000, nullptr, L"м³"},
    {L"пинта|пинты|пинт|pt|pint|pints", C_VOL, 0.473176473, nullptr, L"пинт"},
    {L"чашка|чашки|чашек|cup|cups", C_VOL, 0.2365882365, nullptr, L"чаш."},
    {L"км/ч|кмч|kmh|km/h|kph", C_SPEED, 1 / 3.6, nullptr, L"км/ч"},
    {L"м/с|мс|m/s", C_SPEED, 1, nullptr, L"м/с"},
    {L"mph|миль/ч|миль в час", C_SPEED, 0.44704, nullptr, L"mph"},
    {L"узел|узла|узлов|kn|knot|knots", C_SPEED, 0.514444, nullptr, L"уз."},
    {L"бит|bit|bits", C_DATA, 0.125, nullptr, L"бит"},
    {L"б|байт|байта|байтов|b|byte|bytes", C_DATA, 1, nullptr, L"Б"},
    {L"кб|килобайт|килобайта|килобайтов|kb", C_DATA, 1024.0, nullptr, L"КБ"},
    {L"мб|мегабайт|мегабайта|мегабайтов|mb", C_DATA, 1048576.0, nullptr, L"МБ"},
    {L"гб|гигабайт|гигабайта|гигабайтов|gb", C_DATA, 1073741824.0, nullptr, L"ГБ"},
    {L"тб|терабайт|терабайта|терабайтов|tb", C_DATA, 1099511627776.0, nullptr, L"ТБ"},
    {L"мбит|мегабит|mbit|mbps|мбит/с", C_DATA, 125000.0, nullptr, L"Мбит"},
    {L"мс|миллисекунд|миллисекунда|ms", C_TIME, 0.001, nullptr, L"мс"},
    {L"с|сек|секунда|секунды|секунд|s|sec|second|seconds", C_TIME, 1, nullptr, L"с"},
    {L"мин|минута|минуты|минут|min|minute|minutes", C_TIME, 60, nullptr, L"мин"},
    {L"ч|час|часа|часов|h|hr|hour|hours", C_TIME, 3600, nullptr, L"ч"},
    {L"д|день|дня|дней|сутки|суток|d|day|days", C_TIME, 86400, nullptr, L"дн."},
    {L"неделя|недели|недель|нед|week|weeks", C_TIME, 604800, nullptr, L"нед."},
    {L"год|года|лет|year|years", C_TIME, 31557600, nullptr, L"лет"},
    {L"м2|м²|кв.м|квм|m2", C_AREA, 1, nullptr, L"м²"},
    {L"км2|км²|km2", C_AREA, 1e6, nullptr, L"км²"},
    {L"га|гектар|гектара|гектаров|ha", C_AREA, 1e4, nullptr, L"га"},
    {L"сотка|сотки|соток|сотку", C_AREA, 100, nullptr, L"сот."},
    {L"акр|акра|акров|acre|acres", C_AREA, 4046.8564224, nullptr, L"акр."},
    {L"руб|рубль|рубля|рублей|рубли|р|₽|rub|rur", C_CUR, 0, "RUB", L"₽"},
    {L"usd|$|доллар|доллара|долларов|доллары|бакс|бакса|баксов|dollar|dollars", C_CUR, 0, "USD", L"$"},
    {L"eur|€|евро|euro", C_CUR, 0, "EUR", L"€"},
    {L"cny|юань|юаня|юаней|юани|¥", C_CUR, 0, "CNY", L"¥"},
    {L"kzt|тенге|₸", C_CUR, 0, "KZT", L"₸"},
    {L"uah|гривна|гривны|гривен|₴", C_CUR, 0, "UAH", L"₴"},
    {L"byn|белрубль|белорусских рублей|бел.руб", C_CUR, 0, "BYN", L"BYN"},
    {L"gbp|£|фунт стерлингов|фунтов стерлингов", C_CUR, 0, "GBP", L"£"},
    {L"jpy|иена|иены|иен|йена|йен", C_CUR, 0, "JPY", L"JPY"},
    {L"try|лира|лиры|лир", C_CUR, 0, "TRY", L"TRY"},
    {L"amd|драм|драма|драмов", C_CUR, 0, "AMD", L"AMD"},
    {L"gel|лари", C_CUR, 0, "GEL", L"GEL"},
    {L"uzs|сум|сума|сумов", C_CUR, 0, "UZS", L"UZS"},
    {L"aed|дирхам|дирхама|дирхамов", C_CUR, 0, "AED", L"AED"},
    {L"thb|бат|бата|батов", C_CUR, 0, "THB", L"THB"},
    {L"kgs|сом|сома|сомов", C_CUR, 0, "KGS", L"KGS"},
    {L"btc|биткоин|биткоина|биткоинов|bitcoin", C_CUR, 0, "BTC", L"BTC"},
};
struct Found { const Unit* u; std::string code; };
std::vector<Found> FindUnits(std::wstring n, const std::map<std::string, double>* rates) {
    std::vector<Found> r;
    while (!n.empty() && (n.back() == L' ' || n.back() == L'.' || n.back() == L'?')) n.pop_back();
    while (!n.empty() && n.front() == L' ') n.erase(0, 1);
    n = LowerW(n);
    if (n.empty()) return r;
    for (const Unit& u : kUnits) {
        const wchar_t* s = u.names;
        while (*s) {
            const wchar_t* e = wcschr(s, L'|');
            size_t len = e ? (size_t)(e - s) : wcslen(s);
            if (n.size() == len && n.compare(0, len, s, len) == 0) { r.push_back({&u, u.code ? u.code : ""}); break; }
            s += len + (e ? 1 : 0);
        }
    }
    // любой трёхбуквенный код из таблицы курсов
    if (r.empty() && rates && n.size() == 3) {
        std::string up;
        for (wchar_t c : n) up += (char)towupper(c);
        if (rates->count(up)) {
            static const Unit any = {L"", C_CUR, 0, nullptr, L""};
            r.push_back({&any, up});
        }
    }
    return r;
}
double TempToC(double v, double kind) { return kind == 0 ? v : kind == 1 ? (v - 32) * 5 / 9 : v - 273.15; }
double CToTemp(double c, double kind) { return kind == 0 ? c : kind == 1 ? c * 9 / 5 + 32 : c + 273.15; }
std::wstring ShowName(const Found& f) {
    if (f.u->show && *f.u->show) return f.u->show;
    std::wstring w;
    for (char c : f.code) w += (wchar_t)c;
    return w;
}
}  // namespace

bool UnitConvert(const std::wstring& q0, const std::map<std::string, double>* rates, std::wstring& text, double& value,
                 bool& needRates) {
    needRates = false;
    std::wstring q = LowerW(q0);
    for (auto& c : q) if (c == 0xA0) c = L' ';
    size_t i = 0;
    while (i < q.size() && q[i] == L' ') i++;
    // символ валюты перед числом: $100
    std::wstring pre;
    if (i < q.size() && (q[i] == L'$' || q[i] == 0x20AC || q[i] == 0x20BD || q[i] == 0xA3)) { pre = q.substr(i, 1); i++; }
    size_t ns = i;
    bool neg = false;
    if (i < q.size() && q[i] == L'-') { neg = true; i++; }
    std::wstring num;
    bool dot = false;
    while (i < q.size()) {
        wchar_t c = q[i];
        if (iswdigit(c)) num += c;
        else if ((c == L'.' || c == L',') && !dot && i + 1 < q.size() && iswdigit(q[i + 1])) { num += L'.'; dot = true; }
        else if (c == L' ' && i + 3 < q.size() && iswdigit(q[i + 1]) && iswdigit(q[i + 2]) && iswdigit(q[i + 3]) &&
                 (i + 4 >= q.size() || !iswdigit(q[i + 4])) && !num.empty() && !dot) {}
        else break;
        i++;
    }
    if (num.empty()) return false;
    (void)ns;
    double v = wcstod(num.c_str(), nullptr) * (neg ? -1 : 1);
    if (i < q.size() && (q[i] == L'k' || q[i] == L'к') && (i + 1 >= q.size() || q[i + 1] == L' ') && pre.size()) { v *= 1000; i++; }
    std::wstring rest = q.substr(i);
    // делим на «откуда» и «куда»
    std::wstring from = pre, to;
    static const wchar_t* seps[] = {L" в ", L" во ", L" in ", L" to ", L" -> ", L" → ", L" = ", L"->", L"→"};
    size_t best = std::wstring::npos, bl = 0;
    for (auto s : seps) {
        size_t p = rest.rfind(s);
        if (p != std::wstring::npos && (best == std::wstring::npos || p > best)) { best = p; bl = wcslen(s); }
    }
    if (best != std::wstring::npos) {
        if (from.empty()) from = rest.substr(0, best);
        to = rest.substr(best + bl);
    } else if (from.empty()) from = rest;
    else if (!rest.empty() && rest.find_first_not_of(L' ') != std::wstring::npos) return false;
    auto fs = FindUnits(from, rates);
    if (fs.empty()) return false;
    std::vector<Found> ts;
    if (!to.empty()) {
        ts = FindUnits(to, rates);
        if (ts.empty()) return false;
    } else {
        // без «куда» — только валюты: в рубли (а рубли — в доллары)
        bool cur = false;
        for (auto& f : fs) if (f.u->cat == C_CUR) cur = true;
        if (!cur) return false;
        static const Unit rub = {L"", C_CUR, 0, "RUB", L"₽"}, usd = {L"", C_CUR, 0, "USD", L"$"};
        const Found& f0 = fs[0].u->cat == C_CUR ? fs[0] : fs.back();
        ts.push_back(f0.code == "RUB" ? Found{&usd, "USD"} : Found{&rub, "RUB"});
    }
    for (auto& f : fs)
        for (auto& t : ts) {
            if (f.u->cat != t.u->cat) continue;
            double r;
            switch (f.u->cat) {
                case C_TEMP: r = CToTemp(TempToC(v, f.u->f), t.u->f); break;
                case C_CUR: {
                    if (!rates || rates->empty()) { needRates = true; return true; }
                    auto a = rates->find(f.code), b = rates->find(t.code);
                    if (a == rates->end() || b == rates->end() || a->second <= 0) return false;
                    r = v / a->second * b->second;
                    if (t.code != "BTC") r = std::round(r * 100) / 100;
                    break;
                }
                default: r = v * f.u->f / t.u->f;
            }
            value = r;
            text = FmtNum(v) + L" " + ShowName(f) + L" = " + FmtNum(r) + L" " + ShowName(t);
            return true;
        }
    return false;
}

// ---------------------------------------------------------------- нечёткий поиск
int FuzzyScore(const std::wstring& name, const std::wstring& q) {
    if (q.empty()) return 1;
    if (name.size() < q.size()) {
        // всё равно может совпасть по инициалам — нет, слишком коротко
        return 0;
    }
    if (name.compare(0, q.size(), q) == 0) return 1000 - (int)std::min<size_t>(200, name.size() - q.size());
    auto isSep = [](wchar_t c) { return c == L' ' || c == L'-' || c == L'_' || c == L'.' || c == L'(' || c == L'\\' || c == L'/'; };
    size_t p = name.find(q);
    while (p != std::wstring::npos) {
        if (p > 0 && isSep(name[p - 1])) return 850 - (int)std::min<size_t>(200, name.size() - q.size());
        p = name.find(q, p + 1);
    }
    if (name.find(q) != std::wstring::npos) return 600 - (int)std::min<size_t>(200, name.size());
    // инициалы: «vsc» -> «Visual Studio Code»
    {
        size_t qi = 0;
        for (size_t k = 0; k < name.size() && qi < q.size(); k++)
            if ((k == 0 || isSep(name[k - 1])) && name[k] == q[qi]) qi++;
        if (qi == q.size() && q.size() >= 2) return 500;
    }
    // подпоследовательность
    size_t qi = 0, first = std::wstring::npos, last = 0;
    for (size_t k = 0; k < name.size() && qi < q.size(); k++)
        if (name[k] == q[qi]) { if (first == std::wstring::npos) first = k; last = k; qi++; }
    if (qi < q.size() || q.size() < 3) return 0;
    int spread = (int)(last - first) - (int)q.size();
    return std::max(1, 300 - spread * 10);
}
