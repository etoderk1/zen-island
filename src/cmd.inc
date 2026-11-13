// cmd.inc — командная строка (Alt+Space): программы, файлы, калькулятор, единицы и валюты, команды, буфер.
// Подключается в island.cpp после функций буфера обмена.
// ----------------------------------------------------------------------------

enum CmdResKind { R_CALC, R_CONV, R_CMD, R_APP, R_FILE, R_FOLDER, R_CLIP, R_WEB, R_INFO };
enum CmdAction {
    A_NONE, A_TIMER, A_REMIND, A_SHOT_AREA, A_SHOT_FULL, A_OCR, A_QR, A_PICK, A_REC, A_WIN_DARK, A_WIN_LIGHT, A_ISLE_DARK,
    A_ISLE_LIGHT, A_SETTINGS, A_CLIP, A_TAB_HOME, A_TAB_REC, A_TAB_TEXT, A_TAB_NOTE, A_TAB_WEATHER, A_TAB_CTRL, A_LOCK, A_SLEEP,
    A_REBOOT, A_SHUTDOWN, A_WIFI, A_BT, A_DND, A_VOLUME, A_MUTE, A_PLAY, A_NEXT, A_PREV, A_BIN, A_PLAIN, A_PODS, A_FIREWORKS, A_CONFETTI, A_STYLE
};
struct CmdRes {
    int kind = R_INFO, action = A_NONE;
    std::wstring title, sub, arg;
    double num = 0;
    long long clipId = 0;
};
static std::vector<CmdRes> g_cmdRes;
static std::wstring g_cmdQ;
static int g_cmdSel = 0, g_cmdHover = -1, g_cmdArm = -1;
static double g_cmdArmUntil = 0;
static std::unique_ptr<CmdIndexData> g_index;
static double g_indexT = -1e9;
static std::map<std::string, double> g_rates;
static bool g_ratesAsked = false;
static D2D1_RECT_F g_cmdRows[8];
static int g_cmdRowN = 0;
struct CmdIcon { std::vector<BYTE> px; int w = 0, h = 0; Pic pic; bool tried = false; };
static std::map<std::wstring, CmdIcon> g_cmdIcons;

struct CmdDef { const wchar_t* keys; const wchar_t* title; const wchar_t* sub; int action; };
static const CmdDef kCmds[] = {
    {L"скриншот|скрин|область|снимок|screenshot", L"Скриншот области", L"Ctrl+Alt+S", A_SHOT_AREA},
    {L"весь экран|скриншот экрана|fullscreen", L"Скриншот всего экрана", L"", A_SHOT_FULL},
    {L"текст с экрана|распознать|ocr", L"Текст с экрана", L"Распознать текст в выделенной области", A_OCR},
    {L"qr|кьюар|qr-код", L"QR-код с экрана", L"", A_QR},
    {L"пипетка|цвет|color", L"Пипетка", L"Взять цвет с экрана", A_PICK},
    {L"запись|запись экрана|record", L"Запись экрана", L"Ctrl+Alt+R", A_REC},
    {L"тёмная тема|темная тема|dark", L"Тёмная тема Windows", L"", A_WIN_DARK},
    {L"светлая тема|light", L"Светлая тема Windows", L"", A_WIN_LIGHT},
    {L"тёмный остров|темный остров", L"Тёмный остров", L"", A_ISLE_DARK},
    {L"светлый остров", L"Светлый остров", L"", A_ISLE_LIGHT},
    {L"настройки|settings|параметры острова", L"Настройки острова", L"", A_SETTINGS},
    {L"буфер|история буфера|clipboard", L"История буфера", L"Ctrl+Alt+V", A_CLIP},
    {L"чистый текст|без форматирования", L"Убрать форматирование в буфере", L"", A_PLAIN},
    {L"главная|плеер|музыка", L"Открыть «Главную»", L"", A_TAB_HOME},
    {L"запись экрана|запись|record|obs", L"Открыть «Запись экрана»", L"", A_TAB_REC},
    {L"текст|инструменты текста|раскладка", L"Открыть «Текст»", L"", A_TAB_TEXT},
    {L"погода|weather", L"Погода", L"", A_TAB_WEATHER},
    {L"управление|wi-fi|wifi|вайфай", L"Управление", L"Wi-Fi, Bluetooth, тема, питание", A_TAB_CTRL},
    {L"bluetooth|блютуз|блютус", L"Bluetooth вкл/выкл", L"", A_BT},
    {L"не беспокоить|dnd|тихо", L"Не беспокоить", L"", A_DND},
    {L"громкость|volume|звук", L"Громкость", L"громкость 30", A_VOLUME},
    {L"выключить звук|mute|без звука", L"Выключить / включить звук", L"", A_MUTE},
    {L"пауза|играть|play|pause", L"Пауза / играть", L"", A_PLAY},
    {L"следующий трек|следующая|next|дальше", L"Следующий трек", L"", A_NEXT},
    {L"предыдущий трек|предыдущая|prev|назад", L"Предыдущий трек", L"", A_PREV},
    {L"блокировка|заблокировать|lock", L"Заблокировать компьютер", L"", A_LOCK},
    {L"сон|спящий режим|sleep", L"Спящий режим", L"", A_SLEEP},
    {L"перезагрузка|перезагрузить|restart|reboot", L"Перезагрузка", L"", A_REBOOT},
    {L"выключение|выключить компьютер|выключить пк|shutdown", L"Выключить компьютер", L"", A_SHUTDOWN},
    {L"очистить корзину|корзина", L"Очистить корзину", L"", A_BIN},
    {L"салют|фейерверк|fireworks", L"Салют!", L"Просто потому что можно", A_FIREWORKS},
    {L"конфетти|confetti|ура", L"Конфетти", L"", A_CONFETTI},
    {L"стиль|дизайн|тема острова", L"Стили острова", L"Чёлка, стекло, неон, аврора, пиксель…", A_STYLE},
    {L"наушники|airpods|эирподсы|аирподс|заряд наушников", L"Заряд AirPods", L"Открой кейс рядом с компьютером", A_PODS},
    {L"система|cpu|процессор|видеокарта|gpu|диспетчер|скорость интернета", L"Загрузка системы и скорость сети", L"Вкладка «Управление»", A_TAB_CTRL},
};

static int CmdRowCount() { return std::min((int)g_cmdRes.size(), 8); }
static std::wstring Trim(const std::wstring& s) {
    size_t a = s.find_first_not_of(L" \t"), b = s.find_last_not_of(L" \t");
    return a == std::wstring::npos ? L"" : s.substr(a, b - a + 1);
}
static double ParseDurationSec(const std::wstring& a0) {
    std::wstring a = Trim(LowerW(a0));
    if (a.empty()) return 0;
    int mm = 0, ss = 0;
    if (swscanf(a.c_str(), L"%d:%d", &mm, &ss) == 2) return mm * 60.0 + ss;
    wchar_t* end = nullptr;
    std::wstring num;
    for (wchar_t c : a) num += c == L',' ? L'.' : c;
    double v = wcstod(num.c_str(), &end);
    if (end == num.c_str() || v <= 0) return 0;
    std::wstring u = Trim(end);
    if (!u.empty() && (u[0] == L'с' || u[0] == L's')) return v;
    if (!u.empty() && (u[0] == L'ч' || u[0] == L'h')) return v * 3600;
    return v * 60;
}
static std::wstring FmtMin(double sec) {
    int s = (int)sec;
    wchar_t b[64];
    if (s % 60 == 0) swprintf(b, 64, L"%d мин", s / 60);
    else swprintf(b, 64, L"%d:%02d", s / 60, s % 60);
    return b;
}

static void CmdAdd(std::vector<CmdRes>& v, CmdRes r) { v.push_back(std::move(r)); }
static void CmdSearch() {
    g_cmdRes.clear();
    g_cmdSel = 0;
    g_cmdArm = -1;
    std::wstring q = Trim(g_cmdQ), ql = LowerW(q);
    if (ql.empty()) {
        // пустая строка — подсказки
        CmdRes r;
        r.kind = R_INFO;
        r.title = L"Ищи программы, файлы, папки и историю буфера";
        r.sub = L"2^10 · 15% от 3400 · 100 usd в rub · 5 км в мили · таймер 10 · напомни завтра в 9 …";
        g_cmdRes.push_back(r);
        if (!g_index) { CmdRes w; w.kind = R_INFO; w.title = L"Собираю список программ и файлов…"; g_cmdRes.push_back(w); }
        return;
    }
    std::vector<CmdRes> res;
    // 1. калькулятор
    double v;
    if (CalcEval(q, v)) {
        CmdRes r;
        r.kind = R_CALC;
        r.title = L"= " + FmtNum(v);
        r.sub = q + L" · Enter — скопировать";
        r.num = v;
        CmdAdd(res, r);
    }
    // 2. единицы и валюты
    {
        std::wstring t;
        double val = 0;
        bool needRates = false;
        if (UnitConvert(q, g_rates.empty() ? nullptr : &g_rates, t, val, needRates)) {
            CmdRes r;
            if (needRates) {
                r.kind = R_INFO;
                r.title = L"Загружаю курсы валют…";
                r.sub = L"open.er-api.com, обновляются раз в 6 часов";
                if (!g_ratesAsked) { g_ratesAsked = true; RatesFetchAsync(g_hwnd, DataPath(L"rates.json")); }
            } else {
                r.kind = R_CONV;
                size_t eq = t.find(L" = ");
                r.title = L"= " + (eq == std::wstring::npos ? t : t.substr(eq + 3));
                r.sub = t + L" · Enter — скопировать";
                r.num = val;
            }
            CmdAdd(res, r);
        }
    }
    // 3. команды
    for (const CmdDef& c : kCmds) {
        std::wstring keys = c.keys, best;
        size_t a = 0;
        bool match = false;
        std::wstring arg;
        while (a <= keys.size()) {
            size_t b = keys.find(L'|', a);
            if (b == std::wstring::npos) b = keys.size();
            std::wstring k = keys.substr(a, b - a);
            if (ql.compare(0, k.size(), k) == 0 && (ql.size() == k.size() || ql[k.size()] == L' ')) {
                match = true;
                arg = Trim(q.substr(k.size()));
                break;
            }
            if (ql.size() >= 2 && k.compare(0, ql.size(), ql) == 0) { match = true; break; }
            a = b + 1;
        }
        if (!match) continue;
        CmdRes r;
        r.kind = R_CMD;
        r.action = c.action;
        r.title = c.title;
        r.sub = c.sub;
        r.arg = arg;
        if (c.action == A_TIMER) {
            double sec = ParseDurationSec(arg);
            if (sec > 0) { r.title = L"Таймер на " + FmtMin(sec); r.sub = L"Enter — запустить"; r.num = sec; }
        }
        if (c.action == A_REMIND) {
            Reminder rm;
            std::wstring err;
            if (!arg.empty() && ParseReminder(q, FtNow(), rm, err)) {
                r.title = L"Напомнить: " + rm.text;
                r.sub = (rm.daily ? L"каждый день · " : L"") + ReminderWhen(rm.due, FtNow());
                r.arg = q;
            } else if (!arg.empty()) {
                r.sub = err.empty() ? L"Не понял, когда — например «через 20 минут» или «завтра в 9»" : err;
                r.arg.clear();
            }
        }
        if (c.action == A_VOLUME) {
            double n = arg.empty() ? -1 : wcstod(arg.c_str(), nullptr);
            if (n >= 0 && n <= 100 && !arg.empty()) { r.title = L"Громкость " + std::to_wstring((int)n) + L"%"; r.num = n; }
            else r.num = -1;
        }
        if (c.action == A_REBOOT || c.action == A_SHUTDOWN || c.action == A_SLEEP) r.sub = L"Enter дважды — для подтверждения";
        CmdAdd(res, r);
    }
    // 4. программы и файлы (с исправлением раскладки: ghbdtn -> привет)
    if (g_index) {
        std::wstring alt, err;
        bool latin = true;
        for (wchar_t c : ql) if (c > 127) { latin = false; break; }
        if (!ApplyTextOp(TT_LAYOUT, ql, alt, err)) alt.clear();
        alt = LowerW(alt);
        if (alt == ql) alt.clear();
        (void)latin;
        struct Sc { int s; const CmdEntry* e; };
        std::vector<Sc> apps, files;
        for (auto& e : g_index->apps) {
            int s = std::max(FuzzyScore(e.lower, ql), alt.empty() ? 0 : FuzzyScore(e.lower, alt) - 50);
            if (s > 0) apps.push_back({s, &e});
        }
        if (ql.size() >= 2)
            for (auto& e : g_index->files) {
                int s = std::max(FuzzyScore(e.lower, ql), alt.empty() ? 0 : FuzzyScore(e.lower, alt) - 50);
                if (s >= 500) files.push_back({s + (e.kind == CE_FOLDER ? 20 : 0), &e});
            }
        auto by = [](const Sc& a, const Sc& b) { return a.s != b.s ? a.s > b.s : a.e->mtime > b.e->mtime; };
        std::sort(apps.begin(), apps.end(), by);
        std::sort(files.begin(), files.end(), by);
        int na = 0;
        for (auto& a : apps) {
            if (na++ >= 5) break;
            CmdRes r;
            r.kind = R_APP;
            r.title = a.e->name;
            r.sub = L"Программа";
            r.arg = a.e->target;
            CmdAdd(res, r);
        }
        int nf = 0;
        for (auto& f : files) {
            if (nf++ >= 4) break;
            CmdRes r;
            r.kind = f.e->kind == CE_FOLDER ? R_FOLDER : R_FILE;
            r.title = f.e->name;
            r.sub = f.e->sub;
            r.arg = f.e->target;
            CmdAdd(res, r);
        }
    }
    // путь: D:\claude\…
    if (q.size() >= 3 && q[1] == L':' && (q[2] == L'\\' || q[2] == L'/')) {
        std::wstring p = q;
        for (auto& c : p) if (c == L'/') c = L'\\';
        size_t sl = p.find_last_of(L'\\');
        std::wstring dir = p.substr(0, sl), pre = LowerW(p.substr(sl + 1));
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
        int n = 0;
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (fd.cFileName[0] == L'.' || (fd.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN)) continue;
                std::wstring nm = fd.cFileName;
                if (LowerW(nm).compare(0, pre.size(), pre) != 0) continue;
                CmdRes r;
                r.kind = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? R_FOLDER : R_FILE;
                r.title = nm;
                r.sub = dir;
                r.arg = dir + L"\\" + nm;
                CmdAdd(res, r);
            } while (++n < 400 && res.size() < 12 && FindNextFileW(h, &fd));
            FindClose(h);
        }
    }
    // 5. буфер
    if (ql.size() >= 2 && g_set.clip) {
        int n = 0;
        for (auto& it : g_clipItems) {
            if (it.kind != CK_TEXT && it.kind != CK_LINK) continue;
            if (LowerW(it.preview).find(ql) == std::wstring::npos) continue;
            CmdRes r;
            r.kind = R_CLIP;
            std::wstring t = it.preview;
            for (auto& c : t) if (c == L'\r' || c == L'\n' || c == L'\t') c = L' ';
            r.title = t;
            r.sub = L"Из буфера · Enter — вставить";
            r.clipId = it.id;
            CmdAdd(res, r);
            if (++n >= 2) break;
        }
    }
    // 6. интернет
    {
        CmdRes r;
        r.kind = R_WEB;
        r.title = L"Найти «" + q + L"»";
        r.sub = g_set.search == 1 ? L"Яндекс" : L"Google";
        r.arg = q;
        CmdAdd(res, r);
    }
    if (res.size() > 8) res.resize(8);
    g_cmdRes = std::move(res);
}

static void CmdRequestIndex(bool force) {
    if (force || !g_index || NowSec() - g_indexT > 600) {
        std::vector<std::wstring> extra;
        for (auto& l : g_launch) {
            DWORD a = GetFileAttributesW(l.path.c_str());
            if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) extra.push_back(l.path);
        }
        CmdIndexBuildAsync(g_hwnd, extra);
    }
}
static void OnIndex(CmdIndexData* d) {
    g_index.reset(d);
    g_indexT = NowSec();
    if (g_cmdOpen) { CmdSearch(); Retarget(); g_dirty = true; }
}
static void OnRates(std::map<std::string, double>* m) {
    g_rates = std::move(*m);
    delete m;
    if (g_cmdOpen) { CmdSearch(); Retarget(); g_dirty = true; }
}

static void OpenCmd() {
    if (!g_set.cmd) return;
    if (g_cmdOpen) { SetForegroundWindow(g_hwnd); return; }
    if (g_clipOpen) CloseClip(false);
    g_prevFg = GetForegroundWindow();
    if (g_prevFg == g_hwnd || EditorFocused()) g_prevFg = nullptr;
    if (!g_visible) { g_visible = true; ShowWindow(g_hwnd, SW_SHOWNOACTIVATE); }
    g_cmdOpen = true;
    g_cmdQ.clear();
    g_cmdHover = -1;
    CmdRequestIndex(false);
    if (g_rates.empty() && !g_ratesAsked) { g_ratesAsked = true; RatesFetchAsync(g_hwnd, DataPath(L"rates.json")); }
    CmdSearch();
    SetWindowLongPtrW(g_hwnd, GWL_EXSTYLE, GetWindowLongPtrW(g_hwnd, GWL_EXSTYLE) & ~WS_EX_NOACTIVATE);
    KillTimer(g_hwnd, T_TOAST);
    SetMode(M_CMD);
    SetForegroundWindow(g_hwnd);
    SetFocus(g_hwnd);
    g_caretOn = true;
    SetTimer(g_hwnd, T_CARET, 530, nullptr);
}
static void CloseCmd(bool restoreFocus) {
    if (!g_cmdOpen) return;
    g_cmdOpen = false;
    KillTimer(g_hwnd, T_CARET);
    SetWindowLongPtrW(g_hwnd, GWL_EXSTYLE, GetWindowLongPtrW(g_hwnd, GWL_EXSTYLE) | WS_EX_NOACTIVATE);
    if (g_mode == M_CMD) SetMode(Collapsed());
    if (restoreFocus && g_prevFg && IsWindow(g_prevFg)) SetForegroundWindow(g_prevFg);
    // иконки не держим: следующее открытие заново подгрузит нужные
    if (g_cmdIcons.size() > 120) {
        for (auto& kv : g_cmdIcons) kv.second.pic.Release();
        g_cmdIcons.clear();
    }
}
static void OpenPanelTab(int t) {
    CloseCmd(false);
    SetMode(M_EXPANDED);
    SwitchTab(t);
}
static void RunCmd(int idx, bool ctrl) {
    if (idx < 0 || idx >= (int)g_cmdRes.size()) return;
    CmdRes r = g_cmdRes[idx];
    switch (r.kind) {
        case R_INFO: return;
        case R_CALC:
        case R_CONV: {
            std::wstring s = FmtNumPlain(r.num);
            CopyTextToClipboard(g_hwnd, s);
            CloseCmd(true);
            ShowActionToast(TK_TEXT, L"Скопировано: " + s, L"", TA_NONE, L"", g_accent, 1600);
            return;
        }
        case R_APP:
            CloseCmd(false);
            ShellExecuteW(nullptr, L"open", r.arg.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            return;
        case R_FILE:
        case R_FOLDER:
            CloseCmd(false);
            if (ctrl) SelectInExplorer(r.arg);
            else ShellExecuteW(nullptr, L"open", r.arg.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            return;
        case R_CLIP: {
            for (auto& it : g_clipItems)
                if (it.id == r.clipId) {
                    bool ok = PutToClipboard(it);
                    HWND fg = g_prevFg;
                    CloseCmd(true);
                    if (ok && fg) { g_pasteTries = 0; SetTimer(g_hwnd, T_PASTE, 70, nullptr); }
                    return;
                }
            return;
        }
        case R_WEB: {
            std::wstring url = (g_set.search == 1 ? L"https://ya.ru/search/?text=" : L"https://www.google.com/search?q=") + net::UrlEnc(r.arg);
            CloseCmd(false);
            ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            return;
        }
    }
    // команды
    bool danger = r.action == A_REBOOT || r.action == A_SHUTDOWN || r.action == A_SLEEP;
    if (danger && !(g_cmdArm == idx && NowSec() < g_cmdArmUntil)) {
        g_cmdArm = idx;
        g_cmdArmUntil = NowSec() + 3;
        g_cmdRes[idx].sub = L"Нажми Enter ещё раз";
        g_dirty = true;
        return;
    }
    switch (r.action) {
        case A_TIMER:
            if (r.num <= 0) { g_cmdQ = L"таймер "; CmdSearch(); Retarget(); g_dirty = true; return; }
            CloseCmd(true);
            StartTimer(r.num, 0);
            return;
        case A_REMIND: {
            Reminder rm;
            std::wstring err;
            if (r.arg.empty() || !ParseReminder(r.arg, FtNow(), rm, err)) {
                if (Trim(g_cmdQ).find(L' ') == std::wstring::npos) { g_cmdQ = L"напомни "; CmdSearch(); Retarget(); g_dirty = true; }
                return;
            }
            rm.id = FtNow();
            g_rem.push_back(rm);
            RemSort();
            RemSave();
            CloseCmd(true);
            ShowActionToast(TK_REMIND, rm.text, ReminderWhen(rm.due, FtNow()), TA_NONE, L"", Col(1.f, 0.8f, 0.3f), 2200);
            return;
        }
        case A_SHOT_AREA: CloseCmd(true); RunTool(TM_SHOT_AREA); return;
        case A_SHOT_FULL: CloseCmd(true); RunTool(TM_SHOT_FULL); return;
        case A_OCR: CloseCmd(true); RunTool(TM_OCR); return;
        case A_QR: CloseCmd(true); RunTool(TM_QR); return;
        case A_PICK: CloseCmd(true); RunTool(TM_PICK); return;
        case A_REC: CloseCmd(true); RunTool(TM_REC); return;
        case A_WIN_DARK: CloseCmd(true); SetDarkTheme(true); return;
        case A_WIN_LIGHT: CloseCmd(true); SetDarkTheme(false); return;
        case A_ISLE_DARK: g_set.theme = 0; SaveSettings(); ApplyTheme(); CloseCmd(true); return;
        case A_ISLE_LIGHT: g_set.theme = 1; SaveSettings(); ApplyTheme(); CloseCmd(true); return;
        case A_SETTINGS: OpenPanelTab(TAB_SETTINGS); return;
        case A_CLIP: CloseCmd(false); OpenClip(); return;
        case A_PLAIN: CloseCmd(true); PlainTextClipboard(); return;
        case A_TAB_HOME: OpenPanelTab(TAB_HOME); return;
        case A_TAB_REC: OpenPanelTab(TAB_REC); return;
        case A_TAB_TEXT: OpenPanelTab(TAB_TEXT); return;
        case A_TAB_NOTE: OpenPanelTab(TAB_NOTE); return;
        case A_TAB_WEATHER: OpenPanelTab(TAB_WEATHER); return;
        case A_TAB_CTRL: OpenPanelTab(TAB_CTRL); return;
        case A_BT: CloseCmd(true); CtrlRefreshAsync(3, g_btState != 1); return;
        case A_DND:
            g_set.dnd = !g_set.dnd;
            SaveSettings();
            CloseCmd(true);
            ShowActionToast(TK_TEXT, g_set.dnd ? L"Не беспокоить включено" : L"Не беспокоить выключено", L"", TA_NONE, L"", g_accent, 1500);
            return;
        case A_VOLUME:
            if (r.num < 0) { g_cmdQ = L"громкость "; CmdSearch(); Retarget(); g_dirty = true; return; }
            CloseCmd(true);
            if (g_audio.Ensure()) {
                g_audio.vol->SetMasterVolumeLevelScalar((float)(r.num / 100.0), nullptr);
                g_audio.vol->SetMute(FALSE, nullptr);
                ShowVolumeToast((float)(r.num / 100.0), false);
            }
            return;
        case A_MUTE: CloseCmd(true); ChangeVolume(0, true); return;
        case A_PLAY: CloseCmd(true); g_mw.Command(CMD_TOGGLE); return;
        case A_NEXT: CloseCmd(true); g_mw.Command(CMD_NEXT); return;
        case A_PREV: CloseCmd(true); g_mw.Command(CMD_PREV); return;
        case A_LOCK: CloseCmd(false); PowerAction(0); return;
        case A_SLEEP: CloseCmd(false); PowerAction(1); return;
        case A_REBOOT: CloseCmd(false); PowerAction(2); return;
        case A_SHUTDOWN: CloseCmd(false); PowerAction(3); return;
        case A_PODS: CloseCmd(true); PodsLookup(L"", true); return;
        case A_FIREWORKS: CloseCmd(true); Fireworks(6); return;
        case A_CONFETTI: CloseCmd(true); Confetti(); PlayUi(SN_DONE); return;
        case A_STYLE: OpenPanelTab(TAB_SETTINGS); g_setSec = 1; g_setScroll = g_setScrollT = 0; return;
        case A_BIN:
            CloseCmd(true);
            SHEmptyRecycleBinW(nullptr, nullptr, SHERB_NOCONFIRMATION | SHERB_NOPROGRESSUI);
            ShowActionToast(TK_TEXT, L"Корзина очищена", L"", TA_NONE, L"", g_accent, 1500);
            return;
    }
}

static void CmdChanged() {
    CmdSearch();
    g_caretOn = true;
    Retarget();
    g_dirty = true;
}
static void OnCmdKey(UINT vk) {
    bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    int n = CmdRowCount();
    switch (vk) {
        case VK_ESCAPE:
            if (!g_cmdQ.empty()) { g_cmdQ.clear(); CmdChanged(); }
            else CloseCmd(true);
            return;
        case VK_UP: if (n) g_cmdSel = (g_cmdSel + n - 1) % n; g_cmdArm = -1; break;
        case VK_DOWN: if (n) g_cmdSel = (g_cmdSel + 1) % n; g_cmdArm = -1; break;
        case VK_RETURN: RunCmd(g_cmdSel, ctrl); return;
        case VK_TAB:
            // дополнить путь/название
            if (g_cmdSel < n && (g_cmdRes[g_cmdSel].kind == R_FOLDER || g_cmdRes[g_cmdSel].kind == R_FILE)) {
                g_cmdQ = g_cmdRes[g_cmdSel].arg + (g_cmdRes[g_cmdSel].kind == R_FOLDER ? L"\\" : L"");
                CmdChanged();
            }
            return;
        case VK_BACK:
            if (ctrl) {
                while (!g_cmdQ.empty() && g_cmdQ.back() == L' ') g_cmdQ.pop_back();
                while (!g_cmdQ.empty() && g_cmdQ.back() != L' ' && g_cmdQ.back() != L'\\') g_cmdQ.pop_back();
            } else if (!g_cmdQ.empty()) g_cmdQ.pop_back();
            CmdChanged();
            return;
        case 'V':
            if (ctrl && OpenClipboard(g_hwnd)) {
                if (HANDLE hd = GetClipboardData(CF_UNICODETEXT))
                    if (const wchar_t* p = (const wchar_t*)GlobalLock(hd)) {
                        std::wstring t = p;
                        GlobalUnlock(hd);
                        for (auto& c : t) if (c == L'\r' || c == L'\n' || c == L'\t') c = L' ';
                        g_cmdQ += t.substr(0, 500);
                    }
                CloseClipboard();
                CmdChanged();
            }
            return;
        case 'A': if (ctrl) { g_cmdQ.clear(); CmdChanged(); } return;
    }
    g_caretOn = true;
    g_dirty = true;
}
static void OnCmdChar(wchar_t ch) {
    if (ch < 32 || ch == 127 || (GetKeyState(VK_CONTROL) & 0x8000)) return;
    if (g_cmdQ.size() > 500) return;
    g_cmdQ += ch;
    CmdChanged();
}

// ---- рисование ----
static const CmdIcon* CmdGetIcon(const std::wstring& key, int& budget) {
    auto it = g_cmdIcons.find(key);
    if (it == g_cmdIcons.end()) {
        if (budget <= 0) { g_dirty = true; return nullptr; }  // догрузим в следующем кадре
        budget--;
        CmdIcon ic;
        ic.tried = true;
        LoadShellIcon(key, ic.px, ic.w, ic.h);
        it = g_cmdIcons.emplace(key, std::move(ic)).first;
    }
    CmdIcon& ic = it->second;
    if (!ic.pic.brush && !ic.px.empty()) MakePic(ic.pic, ic.px, ic.w, ic.h);
    return ic.pic.brush ? &ic : nullptr;
}
static void CmdKindIcon(const CmdRes& r, float cx, float cy) {
    D2D1_COLOR_F c = kWhite;
    switch (r.kind) {
        case R_CALC:
            Line(cx - 6, cy - 3, cx + 6, cy - 3, 2.f, c);
            Line(cx - 6, cy + 3, cx + 6, cy + 3, 2.f, c);
            break;
        case R_CONV:
            Line(cx - 7, cy - 3, cx + 7, cy - 3, 1.6f, c); Line(cx + 3, cy - 7, cx + 7, cy - 3, 1.6f, c);
            Line(cx - 7, cy + 4, cx + 7, cy + 4, 1.6f, c); Line(cx - 7, cy + 4, cx - 3, cy + 8, 1.6f, c);
            break;
        case R_CLIP: TabIcon(TAB_CLIP, cx, cy, c); break;
        case R_WEB: IconSearch(cx, cy, c); break;
        case R_FOLDER: IconFolder(cx, cy, c); break;
        case R_FILE: IconPlainText(cx, cy, c); break;
        case R_INFO: Circle(cx, cy, 2.5f, kGray); break;
        case R_CMD:
            switch (r.action) {
                case A_TIMER: StrokeArc(cx, cy, 7, 0.f, 6.2832f, 1.6f, c); Line(cx, cy, cx, cy - 4.5f, 1.6f, c); Line(cx, cy, cx + 3.5f, cy, 1.6f, c); break;
                case A_REMIND: IconBell(cx, cy - 1, c); break;
                case A_SHOT_AREA: DrawToolIcon(TM_SHOT_AREA, cx, cy, c); break;
                case A_SHOT_FULL: DrawToolIcon(TM_SHOT_FULL, cx, cy, c); break;
                case A_OCR: DrawToolIcon(TM_OCR, cx, cy, c); break;
                case A_QR: DrawToolIcon(TM_QR, cx, cy, c); break;
                case A_PICK: DrawToolIcon(TM_PICK, cx, cy, c); break;
                case A_REC: DrawToolIcon(TM_REC, cx, cy, c); break;
                case A_WIN_DARK: case A_WIN_LIGHT: case A_ISLE_DARK: case A_ISLE_LIGHT: case A_DND:
                    IconMoon(cx, cy, c, kBgSolid); break;
                case A_SETTINGS: IconGear(cx, cy, c); break;
                case A_CLIP: case A_PLAIN: TabIcon(TAB_CLIP, cx, cy, c); break;
                case A_TAB_HOME: TabIcon(TAB_HOME, cx, cy, c); break;
                case A_TAB_REC: TabIcon(TAB_REC, cx, cy, c); break;
                case A_TAB_TEXT: TabIcon(TAB_TEXT, cx, cy, c); break;
                case A_TAB_NOTE: TabIcon(TAB_NOTE, cx, cy, c); break;
                case A_TAB_WEATHER: TabIcon(TAB_WEATHER, cx, cy, c); break;
                case A_TAB_CTRL: TabIcon(TAB_CTRL, cx, cy, c); break;
                case A_BT: IconBt(cx, cy, c); break;
                case A_VOLUME: case A_MUTE: IconSpeaker(cx - 7, cy, 13, r.action == A_MUTE); break;
                case A_PLAY: IconPlayPause(cx, cy, 6, 0.5f); break;
                case A_NEXT: IconSkip(cx, cy, 8, true, true); break;
                case A_PREV: IconSkip(cx, cy, 8, false, true); break;
                case A_LOCK: IconLock(cx, cy, c); break;
                case A_SLEEP: IconSleep(cx, cy, c); break;
                case A_REBOOT: IconRestart(cx, cy, c); break;
                case A_SHUTDOWN: IconPower(cx, cy, c); break;
                case A_BIN: IconCross(cx, cy, c); break;
                case A_PODS: IconHeadphones(cx, cy, c); break;
                case A_FIREWORKS: case A_CONFETTI: for (int k = 0; k < 6; k++) Circle(cx + 6 * std::cos(k * 1.047f), cy + 6 * std::sin(k * 1.047f), 1.8f, HueShift(g_accent, k / 6.f)); break;
                case A_STYLE: TabIcon(TAB_HOME, cx, cy, c); break;
                default: Circle(cx, cy, 3, c);
            }
            break;
    }
}
static void DrawCmd(float ox, float oy, float tw, float th) {
    (void)th;
    // строка ввода
    float bx = ox + 22, by = oy + 12, bh = 40;
    IconSearch(bx + 12, by + bh / 2, kGray);
    D2D1_RECT_F tr = RF(bx + 34, by, tw - 44 - 70, bh);
    if (g_cmdQ.empty())
        Text(g_fCmd, L"Программы, файлы, расчёты, команды…", tr, DWRITE_TEXT_ALIGNMENT_LEADING, WithA(kGray, 0.8f));
    else {
        // длинный текст — показываем хвост
        float w = MeasureW(g_fCmd, g_cmdQ), avail = tr.right - tr.left - 6;
        float shift = std::max(0.f, w - avail);
        g_rt->PushAxisAlignedClip(&tr, D2D1_ANTIALIAS_MODE_ALIASED);
        Text(g_fCmd, g_cmdQ, RF(tr.left - shift, tr.top, w + 20, bh), DWRITE_TEXT_ALIGNMENT_LEADING, kWhite);
        g_rt->PopAxisAlignedClip();
        if (g_caretOn && GetForegroundWindow() == g_hwnd) {
            float cx = tr.left + std::min(w, avail) + 2;
            RRect(RF(cx, by + 10, 2, bh - 20), 1, g_accent);
        }
    }
    if (g_cmdQ.empty() && g_caretOn && GetForegroundWindow() == g_hwnd) RRect(RF(tr.left, by + 10, 2, bh - 20), 1, g_accent);
    Text(g_fSmall, L"Esc", RF(ox + tw - 66, by, 44, bh), DWRITE_TEXT_ALIGNMENT_TRAILING, WithA(kGray, 0.7f));
    int n = CmdRowCount();
    g_cmdRowN = n;
    if (!n) return;
    RRect(RF(ox + 22, oy + 62, tw - 44, 1), 0.5f, Ov(0.08f));
    int budget = 2;  // не больше двух новых иконок за кадр — ввод не тормозит
    for (int i = 0; i < n; i++) {
        const CmdRes& r = g_cmdRes[i];
        D2D1_RECT_F row = RF(ox + 12, oy + 70 + i * 48, tw - 24, 46);
        g_cmdRows[i] = row;
        bool sel = i == g_cmdSel && r.kind != R_INFO;
        if (sel) RRect(row, 14, Ov(0.10f));
        else if (i == g_cmdHover && r.kind != R_INFO) RRect(row, 14, Ov(0.05f));
        float icx = row.left + 26, icy = (row.top + row.bottom) / 2;
        const CmdIcon* ic = nullptr;
        if (r.kind == R_APP || r.kind == R_FILE || r.kind == R_FOLDER) ic = CmdGetIcon(r.arg, budget);
        if (ic) DrawPicRounded(ic->pic, RF(icx - 15, icy - 15, 30, 30), 6, 1.f);
        else {
            if (r.kind != R_INFO) RRect(RF(icx - 16, icy - 16, 32, 32), 9, r.kind == R_CALC || r.kind == R_CONV ? WithA(g_accent, 0.35f) : Ov(0.08f));
            CmdKindIcon(r, icx, icy);
        }
        float tx = row.left + 54, tw2 = row.right - tx - (sel ? 60 : 14);
        IDWriteTextFormat* tf = (r.kind == R_CALC || r.kind == R_CONV) ? g_fCmd : g_fToast;
        if (r.sub.empty()) Text(tf, r.title, RF(tx, row.top, tw2, row.bottom - row.top), DWRITE_TEXT_ALIGNMENT_LEADING, kWhite);
        else {
            Text(tf, r.title, RF(tx, row.top + 3, tw2, 22), DWRITE_TEXT_ALIGNMENT_LEADING, r.kind == R_INFO ? kGray : kWhite);
            Text(g_fSmall, r.sub, RF(tx, row.top + 25, tw2, 17), DWRITE_TEXT_ALIGNMENT_LEADING, g_cmdArm == i ? Col(1.f, 0.45f, 0.4f) : kGray);
        }
        if (sel) Text(g_fSmall, L"Enter ↵", RF(row.right - 70, row.top, 60, row.bottom - row.top), DWRITE_TEXT_ALIGNMENT_TRAILING, kGray);
    }
}
static int CmdHit(int x, int y) {
    for (int i = 0; i < g_cmdRowN; i++)
        if (InRect(g_cmdRows[i], x, y)) return i;
    return -1;
}
