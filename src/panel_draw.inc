// panel_draw.inc — раскрытый остров: вкладки, карточки. Подключается внутрь island.cpp.
// ----------------------------------------------------------------------------

enum Tab { TAB_HOME, TAB_CLIP, TAB_REC, TAB_TEXT, TAB_NOTE, TAB_WEATHER, TAB_CTRL, TAB_COUNT };
static const int TAB_SETTINGS = TAB_COUNT;  // открывается шестерёнкой, в полосе вкладок не показывается
static int g_tab = TAB_HOME;
static bool g_pinned = false;
static const wchar_t* kTabNames[TAB_COUNT] = {L"Главная", L"Буфер", L"Запись", L"Текст", L"Заметка", L"Погода", L"Управление"};

enum HitId {
    H_NONE, H_TAB, H_PIN, H_SETTINGS, H_VOL, H_DEVICE, H_QA, H_LAUNCH, H_LAUNCHADD, H_SHELFTILE, H_SHELFALL, H_SHELFCLEAR,
    H_SHELFX, H_TTOOL, H_TT_PASTE, H_TT_COPY, H_TT_CLEAR, H_NOTEFMT, H_TPRESET, H_TPAUSE, H_TPLUS, H_TRESET, H_REMDEL,
    H_WXREFRESH, H_CTRL, H_POWER, H_TTSCROLL,
    H_SNAV, H_SCTL, H_SSLIDER, H_SCHOICE, H_SSWATCH, H_STABVIS, H_STABUP, H_STABDN, H_SSCROLL, H_SEARCHBTN, H_SSTYLE, H_EDFOCUS, H_IEDIT, H_SBAR, H_MEDIASRC,
    H_APPPICK, H_APPCLOSE, H_APPBROWSE, H_RECUI, H_SPET, H_SCUST
};
struct Hit { D2D1_RECT_F r; int id, arg; };
static std::vector<Hit> g_hits;
static void AddHit(D2D1_RECT_F r, int id, int arg = 0) { g_hits.push_back({r, id, arg}); }
static bool Hot(int id, int arg = 0) { return g_hotId == id && g_hotArg == arg; }
static const Hit* HitAt(int x, int y) {
    float fx = x / g_scale, fy = y / g_scale;
    for (int i = (int)g_hits.size() - 1; i >= 0; i--) {
        const D2D1_RECT_F& r = g_hits[i].r;
        if (fx >= r.left && fx <= r.right && fy >= r.top && fy <= r.bottom) return &g_hits[i];
    }
    return nullptr;
}

// ---- Поле ввода, нарисованное прямо на острове (без отдельных окон: всегда видно, что пишешь) ----
enum { IE_CITY, IE_ICS, IE_REMIND, IE_REPLY, IE_EXCLUDE, IE_APPS, IE_COUNT };
struct IEdit { std::wstring text; int caret = 0; bool all = false; float scroll = 0; };
static IEdit g_ie[IE_COUNT];
static void IECommit(int id, bool enter);  // что делать с текстом (ниже, в panel_logic)
static void AppPickClose();
static void IEFocus(int id) {
    if (id < 0 || id >= IE_COUNT) return;
    g_ieFocus = id;
    g_ie[id].caret = (int)g_ie[id].text.size();
    g_ie[id].all = false;
    SetWindowLongPtrW(g_hwnd, GWL_EXSTYLE, GetWindowLongPtrW(g_hwnd, GWL_EXSTYLE) & ~WS_EX_NOACTIVATE);
    SetForegroundWindow(g_hwnd);
    SetFocus(g_hwnd);
    g_caretOn = true;
    SetTimer(g_hwnd, T_CARET, 530, nullptr);
    g_dirty = true;
}
static void IEBlur(bool commit) {
    if (g_ieFocus < 0) return;
    int id = g_ieFocus;
    g_ieFocus = -1;
    if (!g_clipOpen && !g_cmdOpen) {
        KillTimer(g_hwnd, T_CARET);
        SetWindowLongPtrW(g_hwnd, GWL_EXSTYLE, GetWindowLongPtrW(g_hwnd, GWL_EXSTYLE) | WS_EX_NOACTIVATE);
    }
    if (commit) IECommit(id, false);
    g_dirty = true;
}
static void IEPaste(IEdit& e) {
    std::wstring t;
    if (OpenClipboard(g_hwnd)) {
        if (HANDLE hd = GetClipboardData(CF_UNICODETEXT))
            if (const wchar_t* p = (const wchar_t*)GlobalLock(hd)) { t = p; GlobalUnlock(hd); }
        CloseClipboard();
    }
    for (auto& c : t) if (c == L'\r' || c == L'\n' || c == L'\t') c = L' ';
    if (e.all) { e.text.clear(); e.caret = 0; e.all = false; }
    e.text.insert(e.caret, t.substr(0, 2000));
    e.caret += (int)std::min<size_t>(t.size(), 2000);
}
static bool IEKey(UINT vk) {
    if (g_ieFocus < 0) return false;
    IEdit& e = g_ie[g_ieFocus];
    bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    int n = (int)e.text.size();
    switch (vk) {
        case VK_RETURN: IECommit(g_ieFocus, true); break;
        case VK_ESCAPE: if (g_ieFocus == IE_APPS) { AppPickClose(); return true; } IEBlur(true); break;
        case VK_LEFT: e.all = false; if (e.caret > 0) e.caret--; if (ctrl) while (e.caret > 0 && e.text[e.caret - 1] != L' ') e.caret--; break;
        case VK_RIGHT: e.all = false; if (e.caret < n) e.caret++; if (ctrl) while (e.caret < n && e.text[e.caret] != L' ') e.caret++; break;
        case VK_HOME: e.all = false; e.caret = 0; break;
        case VK_END: e.all = false; e.caret = n; break;
        case VK_BACK:
            if (e.all) { e.text.clear(); e.caret = 0; e.all = false; break; }
            if (ctrl) { int a = e.caret; while (a > 0 && e.text[a - 1] == L' ') a--; while (a > 0 && e.text[a - 1] != L' ') a--; e.text.erase(a, e.caret - a); e.caret = a; }
            else if (e.caret > 0) { e.text.erase(e.caret - 1, 1); e.caret--; }
            break;
        case VK_DELETE:
            if (e.all) { e.text.clear(); e.caret = 0; e.all = false; }
            else if (e.caret < n) e.text.erase(e.caret, 1);
            break;
        case 'A': if (ctrl) { e.all = true; e.caret = n; } break;
        case 'C': if (ctrl && !e.text.empty()) CopyTextToClipboard(g_hwnd, e.text); break;
        case 'X': if (ctrl && e.all) { CopyTextToClipboard(g_hwnd, e.text); e.text.clear(); e.caret = 0; e.all = false; } break;
        case 'V': if (ctrl) IEPaste(e); break;
        default: return true;
    }
    g_caretOn = true;
    g_dirty = true;
    return true;
}
static void IEChar(wchar_t ch) {
    if (g_ieFocus < 0 || ch < 32 || ch == 127 || (GetKeyState(VK_CONTROL) & 0x8000)) return;
    IEdit& e = g_ie[g_ieFocus];
    if (e.all) { e.text.clear(); e.caret = 0; e.all = false; }
    if (e.text.size() > 2000) return;
    e.text.insert(e.caret, 1, ch);
    e.caret++;
    g_caretOn = true;
    g_dirty = true;
}
static void DrawIEdit(int id, D2D1_RECT_F box, const wchar_t* placeholder, float rad = 10) {
    IEdit& e = g_ie[id];
    bool f = g_ieFocus == id;
    RRect(box, rad, kEdCol);
    StrokeRRect(box, rad, f ? 1.6f : 1.f, f ? WithA(g_accent, 0.9f) : Ov(0.10f));
    D2D1_RECT_F tr = RF(box.left + 12, box.top, box.right - box.left - 24, box.bottom - box.top);
    if (e.text.empty() && !f) {
        Text(g_fClip, placeholder, tr, DWRITE_TEXT_ALIGNMENT_LEADING, WithA(kGray, 0.7f));
    } else {
        float cw = MeasureW(g_fClip, e.text.substr(0, e.caret)), tw = MeasureW(g_fClip, e.text), avail = tr.right - tr.left - 4;
        // держим каретку в поле зрения
        if (cw - e.scroll > avail) e.scroll = cw - avail;
        if (cw - e.scroll < 0) e.scroll = cw;
        if (tw - e.scroll < avail) e.scroll = std::max(0.f, tw - avail);
        g_rt->PushAxisAlignedClip(&tr, D2D1_ANTIALIAS_MODE_ALIASED);
        if (e.all && !e.text.empty()) RRect(RF(tr.left - e.scroll, tr.top + 7, tw, tr.bottom - tr.top - 14), 3, WithA(g_accent, 0.35f));
        if (e.text.empty()) Text(g_fClip, placeholder, tr, DWRITE_TEXT_ALIGNMENT_LEADING, WithA(kGray, 0.5f));
        else Text(g_fClip, e.text, RF(tr.left - e.scroll, tr.top, tw + 40, tr.bottom - tr.top), DWRITE_TEXT_ALIGNMENT_LEADING, kWhite);
        if (f && g_caretOn) RRect(RF(tr.left + cw - e.scroll, tr.top + 8, 1.6f, tr.bottom - tr.top - 16), 0.8f, g_accent);
        g_rt->PopAxisAlignedClip();
    }
    AddHit(box, H_IEDIT, id);
}

// Лаунчер
struct LaunchItem { std::wstring path, name; Pic icon; std::vector<BYTE> px; int iw = 0, ih = 0; };
static std::vector<LaunchItem> g_launch;
// Состояния «Управления» (читаются в фоне)
static bool g_ethUp = false;
static int g_wifiState = -2, g_btState = -2;  // -2 неизвестно, -1 нет, 0/1
static std::wstring g_wifiName;
static bool g_darkTheme = true;
static int g_powerArm = -1;          // кнопка питания ждёт второго нажатия
static double g_powerArmUntil = 0;
// Текст и заметка: окна-редакторы (RichEdit) поверх острова
static HWND g_hostText, g_hostNote, g_hostRemind, g_hostCity, g_hostIcs;
static HWND g_reText, g_reNote, g_reRemind, g_reCity, g_reIcs;
static D2D1_RECT_F g_rcText = {}, g_rcNote = {}, g_rcRemind = {}, g_rcCity = {}, g_rcIcs = {};
static int g_lyrOffUi = 0, g_clipLimUi = 4;
static std::wstring g_cmdHotkeyText;
static void DrawSettingsTab(D2D1_RECT_F c);
static int g_ttScroll = 0;
static std::wstring g_textStats, g_textErr, g_remErr;
static double g_textErrUntil = 0, g_remErrUntil = 0;
// Напоминания
static std::vector<Reminder> g_rem;
// Громкость (для слайдера)
static float g_volUi = -1;
static bool g_volDrag = false;
static D2D1_RECT_F g_volTrack = {};
static std::wstring g_outName;

static D2D1_COLOR_F Sky(float a) { return Col(1, 1, 1, a); }  // поверх «неба» в погоде — всегда белым

// ---- иконки вкладок ----
static void TabIcon(int t, float cx, float cy, D2D1_COLOR_F c) {
    switch (t) {
        case TAB_HOME:
            Line(cx - 7, cy - 1, cx, cy - 7, 1.6f, c); Line(cx, cy - 7, cx + 7, cy - 1, 1.6f, c);
            StrokeRRect(RF(cx - 5, cy - 2, 10, 9), 1.5f, 1.6f, c);
            break;
        case TAB_CLIP:
            StrokeRRect(RF(cx - 6, cy - 6, 12, 14), 2, 1.6f, c);
            RRect(RF(cx - 3, cy - 8, 6, 3), 1, c);
            Line(cx - 3, cy, cx + 3, cy, 1.3f, c); Line(cx - 3, cy + 3.5f, cx + 2, cy + 3.5f, 1.3f, c);
            break;
        case TAB_REC: DrawToolIcon(TM_REC, cx, cy, c); break;
        case TAB_TEXT:
            Line(cx - 6, cy - 6, cx + 6, cy - 6, 1.8f, c); Line(cx, cy - 6, cx, cy + 7, 1.8f, c);
            break;
        case TAB_NOTE:
            StrokeRRect(RF(cx - 6, cy - 7, 12, 14), 2, 1.6f, c);
            Line(cx - 3, cy - 2, cx + 3, cy - 2, 1.3f, c); Line(cx - 3, cy + 2, cx + 3, cy + 2, 1.3f, c);
            break;
        case TAB_WEATHER: DrawWx(WX_PARTLY, true, cx, cy, 0.8f, kBgSolid); break;
        case TAB_CTRL:
            for (int i = 0; i < 3; i++) {
                float y = cy - 5 + i * 5;
                Line(cx - 7, y, cx + 7, y, 1.5f, c);
                Circle(cx + (i == 1 ? 3.f : -3.f), y, 2.2f, c);
            }
            break;
    }
}
static void IconGear(float cx, float cy, D2D1_COLOR_F c) {
    StrokeArc(cx, cy, 5, 0.f, 6.2f, 1.8f, c);
    for (int i = 0; i < 6; i++) {
        float a = i * 3.14159f / 3;
        Line(cx + 6 * std::cos(a), cy + 6 * std::sin(a), cx + 8.5f * std::cos(a), cy + 8.5f * std::sin(a), 2.4f, c);
    }
}
static void IconPinTab(float cx, float cy, D2D1_COLOR_F c) {
    SetColor(c);
    D2D1_POINT_2F p[4] = {Pt(cx - 4, cy - 7), Pt(cx + 4, cy - 7), Pt(cx + 3, cy + 1), Pt(cx - 3, cy + 1)};
    FillPoly(p, 4);
    Line(cx - 6, cy + 1, cx + 6, cy + 1, 1.8f, c);
    Line(cx, cy + 1, cx, cy + 8, 1.6f, c);
}
static void IconFolder(float cx, float cy, D2D1_COLOR_F c) {
    StrokeRRect(RF(cx - 8, cy - 5, 16, 12), 2, 1.6f, c);
    Line(cx - 8, cy - 2, cx - 1, cy - 2, 1.4f, c);
}
static void IconExternal(float cx, float cy, D2D1_COLOR_F c) {
    StrokeRRect(RF(cx - 7, cy - 5, 12, 12), 2, 1.5f, c);
    Line(cx - 1, cy + 1, cx + 7, cy - 7, 1.6f, c);
    Line(cx + 2, cy - 7, cx + 7, cy - 7, 1.6f, c);
    Line(cx + 7, cy - 7, cx + 7, cy - 2, 1.6f, c);
}
static void IconCheck(float cx, float cy, D2D1_COLOR_F c) {
    Line(cx - 6, cy, cx - 2, cy + 4, 2.f, c);
    Line(cx - 2, cy + 4, cx + 6, cy - 5, 2.f, c);
}
static void IconBell(float cx, float cy, D2D1_COLOR_F c) {
    StrokeArc(cx, cy - 1, 5.5f, 3.14159f, 6.28318f, 1.7f, c);
    Line(cx - 5.5f, cy - 1, cx - 6.5f, cy + 5, 1.7f, c);
    Line(cx + 5.5f, cy - 1, cx + 6.5f, cy + 5, 1.7f, c);
    Line(cx - 7.5f, cy + 5, cx + 7.5f, cy + 5, 1.7f, c);
    Circle(cx, cy + 8, 1.6f, c);
}
static void IconWifi(float cx, float cy, D2D1_COLOR_F c) {
    for (int i = 0; i < 3; i++) StrokeArc(cx, cy + 5, 4.f + i * 3.6f, 3.93f, 5.5f, 1.8f, c);
    Circle(cx, cy + 5, 1.6f, c);
}
static void IconBt(float cx, float cy, D2D1_COLOR_F c) {
    Line(cx - 1, cy - 8, cx - 1, cy + 8, 1.7f, c);
    Line(cx - 1, cy - 8, cx + 5, cy - 3, 1.7f, c); Line(cx + 5, cy - 3, cx - 6, cy + 4, 1.7f, c);
    Line(cx - 1, cy + 8, cx + 5, cy + 3, 1.7f, c); Line(cx + 5, cy + 3, cx - 6, cy - 4, 1.7f, c);
}
static void IconMoon(float cx, float cy, D2D1_COLOR_F c, D2D1_COLOR_F bg) {
    Circle(cx, cy, 7, c);
    Circle(cx + 3.2f, cy - 2.6f, 6, bg);
}
static void IconLock(float cx, float cy, D2D1_COLOR_F c) {
    StrokeArc(cx, cy - 2, 4.5f, 3.14159f, 6.28318f, 1.7f, c);
    Line(cx - 4.5f, cy - 2, cx - 4.5f, cy, 1.7f, c); Line(cx + 4.5f, cy - 2, cx + 4.5f, cy, 1.7f, c);
    RRect(RF(cx - 6.5f, cy, 13, 9), 2, c);
}
static void IconPower(float cx, float cy, D2D1_COLOR_F c) {
    StrokeArc(cx, cy + 1, 7, -1.0f, 4.14f, 1.8f, c);
    Line(cx, cy - 8, cx, cy, 1.8f, c);
}
static void IconRestart(float cx, float cy, D2D1_COLOR_F c) {
    StrokeArc(cx, cy, 7, -0.6f, 4.6f, 1.8f, c);
    SetColor(c);
    D2D1_POINT_2F p[3] = {Pt(cx + 7.5f, cy - 7), Pt(cx + 8, cy - 1), Pt(cx + 2.5f, cy - 3)};
    FillPoly(p, 3);
}
static void IconSleep(float cx, float cy, D2D1_COLOR_F c) { IconMoon(cx, cy, c, Lerp(kBgSolid, kWhite, 0.06f)); }
static void IconPlainText(float cx, float cy, D2D1_COLOR_F c) {
    StrokeRRect(RF(cx - 7, cy - 6, 14, 12), 2, 1.5f, c);
    Line(cx - 4, cy - 1, cx + 4, cy - 1, 1.4f, c);
    Line(cx - 4, cy + 2.5f, cx + 1, cy + 2.5f, 1.4f, c);
}

// ---- верхняя полоса вкладок ----
static void DrawTabBar(float ox, float oy, float tw) {
    float x = ox + 22, y = oy + 14, h = 40;
    for (int pos = 0; pos < TAB_COUNT; pos++) {
        int t = g_set.tabOrder[pos] - L'0';
        if (t < 0 || t >= TAB_COUNT || (g_set.tabHidden & (1 << t))) continue;
        bool active = (g_mode == M_CLIP ? TAB_CLIP : g_tab) == t;
        float w = active ? 40 + MeasureW(g_fToast, kTabNames[t]) + 16 : 44;
        D2D1_RECT_F r = RF(x, y, w, h);
        AddHit(r, H_TAB, t);
        if (active) RRect(r, 20, Ov(0.11f));
        else if (Hot(H_TAB, t)) RRect(r, 20, Ov(0.06f));
        D2D1_COLOR_F c = active ? kWhite : WithA(kGray, 0.95f);
        TabIcon(t, x + 22, y + h / 2, c);
        if (active) Text(g_fToast, kTabNames[t], RF(x + 38, y, w - 40, h), DWRITE_TEXT_ALIGNMENT_LEADING, kWhite);
        x += w + 6;
    }
    // справа: закрепить и настройки
    float rx = ox + tw - 22 - 40;
    D2D1_RECT_F rs = RF(rx, y, 40, 40), rp = RF(rx - 46, y, 40, 40), rq = RF(rx - 92, y, 40, 40);
    bool setAct = g_tab == TAB_SETTINGS && g_mode == M_EXPANDED;
    AddHit(rs, H_SETTINGS);
    AddHit(rp, H_PIN);
    if (g_set.cmd) AddHit(rq, H_SEARCHBTN);
    if (setAct || Hot(H_SETTINGS)) Circle(rx + 20, y + 20, 20, Ov(setAct ? 0.14f : 0.08f));
    if (g_pinned || Hot(H_PIN)) Circle(rx - 26, y + 20, 20, Ov(g_pinned ? 0.14f : 0.08f));
    IconGear(rx + 20, y + 20, setAct ? kWhite : kGray);
    IconPinTab(rx - 26, y + 19, g_pinned ? kWhite : kGray);
    if (g_set.cmd) {
        if (Hot(H_SEARCHBTN)) Circle(rx - 72, y + 20, 20, Ov(0.08f));
        IconSearch(rx - 72, y + 20, kGray);
    }
}

// ---- Главная ----
static void DrawPlayerCard(Layout& L, D2D1_RECT_F c) {
    float cw = c.right - c.left, ch = c.bottom - c.top;
    // карточка с лёгким оттенком цвета обложки
    ID2D1LinearGradientBrush* gb = nullptr;
    D2D1_GRADIENT_STOP gs[2] = {{0.f, WithA(g_accent, 0.22f)}, {1.f, Ov(0.04f)}};
    ID2D1GradientStopCollection* col = nullptr;
    g_rt->CreateGradientStopCollection(gs, 2, D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP, &col);
    D2D1_LINEAR_GRADIENT_BRUSH_PROPERTIES lg = {Pt(c.left, c.top), Pt(c.right, c.bottom)};
    if (col) g_rt->CreateLinearGradientBrush(&lg, nullptr, col, &gb);
    D2D1_ROUNDED_RECT rr = {c, 22, 22};
    if (gb) g_rt->FillRoundedRectangle(&rr, gb); else RRect(c, 22, kCard);
    SafeRelease(gb);
    SafeRelease(col);
    if (!g_media.has) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        wchar_t t[16];
        swprintf(t, 16, L"%02d:%02d", st.wHour, st.wMinute);
        Text(g_fBig, t, RF(c.left + 24, c.top + 26, cw - 48, 64), DWRITE_TEXT_ALIGNMENT_LEADING, kWhite);
        Text(g_fTitle, L"Сейчас ничего не играет", RF(c.left + 26, c.top + 98, cw - 48, 24), DWRITE_TEXT_ALIGNMENT_LEADING, kGray);
        Text(g_fSmall, L"Включи музыку в Spotify, Яндекс Музыке или браузере — плеер появится здесь", RF(c.left + 26, c.top + 124, cw - 48, 34),
             DWRITE_TEXT_ALIGNMENT_LEADING, WithA(kGray, 0.8f));
        return;
    }
    float cs = ch - 36, cb = 1 + 0.035f * CoverBreath();
    D2D1_RECT_F cvr = RF(c.left + 18 + cs / 2 - cs * cb / 2, c.top + 18 + cs / 2 - cs * cb / 2, cs * cb, cs * cb);
    CoverGlow(cvr, 18);
    DrawCover(cvr, 18);
    if (g_set.critter > 0) DrawCritter(g_set.critter, c.right - 30, c.top + 30, 26);
    float tx = c.left + 18 + cs + 20, tw = c.right - 18 - tx;
    std::wstring src = g_media.appName;
    if (!src.empty()) CharUpperBuffW(&src[0], (DWORD)src.size());
    Text(g_fSmall, src, RF(tx, c.top + 20, tw, 16), DWRITE_TEXT_ALIGNMENT_LEADING, WithA(kGray, 0.85f));
    MarqueeText(g_mqTitle, g_fPlayerTitle, g_media.title, RF(tx, c.top + 38, tw, 28), kWhite);
    MarqueeText(g_mqArtist, g_fArtist, g_media.artist, RF(tx, c.top + 66, tw, 20), kGray);
    // прогресс
    INT64 dur = Duration();
    float py = c.top + 102;
    L.hasProg = false;
    if (dur > 10000000LL) {
        INT64 el = g_dragSeek ? (INT64)(g_dragFrac * (double)dur) : CurElapsed();
        float frac = g_dragSeek ? g_dragFrac : Clamp((float)((double)el / (double)dur), 0.f, 1.f);
        float hv = (float)Clamp(g_progHov.x, 0.0, 1.2), thk = 4.f + 2.f * hv;
        RRect(RF(tx, py - thk / 2, tw, thk), thk / 2, Ov(0.16f));
        if (frac > 0) RRect(RF(tx, py - thk / 2, std::max(thk, tw * frac), thk), thk / 2, Lerp(g_accent, kWhite, 0.15f));
        if (hv > 0.02f) Circle(tx + tw * frac, py, 6.f * std::min(1.f, hv), kWhite);
        Text(g_fSmall, FmtDur(el), RF(tx, py + 6, 60, 16), DWRITE_TEXT_ALIGNMENT_LEADING, kGray);
        Text(g_fSmall, L"-" + FmtDur(dur - el), RF(tx + tw - 60, py + 6, 60, 16), DWRITE_TEXT_ALIGNMENT_TRAILING, kGray);
        L.hasProg = true;
        L.progX0 = tx; L.progX1 = tx + tw; L.progY = py;
    }
    // кнопки
    float cx = tx + tw / 2, cy = c.bottom - 40;
    float bx[3] = {cx - 66, cx, cx + 66}, br[3] = {20, 27, 20};
    for (int i = 0; i < 3; i++) {
        AddBtn(L, bx[i], cy, br[i]);
        float hv = (float)Clamp(g_btnHov[i].x, 0.0, 1.0);
        if (i != 1 && hv > 0.01f) Circle(bx[i], cy, br[i] * (float)g_btnScale[i].x, Ov(0.1f * hv));
    }
    float s1 = (float)g_btnScale[1].x;
    Circle(cx, cy, 26 * s1, kWhite);
    // чёрная иконка на белой кнопке
    float m = (float)Clamp(g_playMorph.x, 0.0, 1.0), k = 10 * s1;
    if (m < 0.99f) {
        SetColor(WithA(kBgSolid, 1 - m));
        D2D1_POINT_2F p[3] = {Pt(cx - k * 0.45f, cy - k * 0.8f), Pt(cx + k * 0.85f, cy), Pt(cx - k * 0.45f, cy + k * 0.8f)};
        FillPoly(p, 3);
    }
    if (m > 0.01f) {
        RRect(RF(cx - k * 0.55f, cy - k * 0.75f, k * 0.38f, k * 1.5f), k * 0.1f, WithA(kBgSolid, m));
        RRect(RF(cx + k * 0.17f, cy - k * 0.75f, k * 0.38f, k * 1.5f), k * 0.1f, WithA(kBgSolid, m));
    }
    IconSkip(bx[0], cy, 10 * (float)g_btnScale[0].x, false, g_media.canPrev);
    IconSkip(bx[2], cy, 10 * (float)g_btnScale[2].x, true, g_media.canNext);
}
static void DrawVolumeCard(D2D1_RECT_F c) {
    RRect(c, 18, kCard);
    float cy = (c.top + c.bottom) / 2;
    IconSpeaker(c.left + 18, cy, 14, g_muted || g_volUi <= 0.001f);
    float devW = std::min(170.f, 46 + MeasureW(g_fSmall, g_outName));
    D2D1_RECT_F dev = RF(c.right - 10 - devW, c.top + 9, devW, c.bottom - c.top - 18);
    float x0 = c.left + 48, x1 = dev.left - 52;
    g_volTrack = RF(x0, cy - 10, x1 - x0, 20);
    AddHit(g_volTrack, H_VOL);
    float v = g_volUi < 0 ? g_vol : g_volUi;
    RRect(RF(x0, cy - 3, x1 - x0, 6), 3, Ov(0.16f));
    if (v > 0 && !g_muted) RRect(RF(x0, cy - 3, std::max(6.f, (x1 - x0) * v), 6), 3, kWhite);
    if (Hot(H_VOL) || g_volDrag) Circle(x0 + (x1 - x0) * v, cy, 7, kWhite);
    Text(g_fSmall, std::to_wstring((int)std::lround(v * 100)), RF(x1 + 8, c.top, 34, c.bottom - c.top), DWRITE_TEXT_ALIGNMENT_TRAILING, kGray);
    AddHit(dev, H_DEVICE);
    RRect(dev, 12, Hot(H_DEVICE) ? kCardHot : Ov(0.07f));
    StrokeRRect(RF(dev.left + 10, cy - 7, 10, 14), 2, 1.3f, kGray);
    Circle(dev.left + 15, cy + 2, 2, kGray);
    Text(g_fSmall, g_outName.empty() ? L"Устройство" : g_outName, RF(dev.left + 26, dev.top, devW - 46, dev.bottom - dev.top),
         DWRITE_TEXT_ALIGNMENT_LEADING, kWhite);
    Line(dev.right - 16, cy - 2, dev.right - 12, cy + 2, 1.4f, kGray);
    Line(dev.right - 12, cy + 2, dev.right - 8, cy - 2, 1.4f, kGray);
}
static const wchar_t* kQaNames[8] = {L"Область", L"Весь экран", L"Текст с экрана", L"QR-код", L"Пипетка", L"Чистый текст", L"Запись экрана", L"Буфер обмена"};
static void DrawQuickActions(D2D1_RECT_F c) {
    float gap = 10, bw = (c.right - c.left - gap) / 2, bh = (c.bottom - c.top - 3 * gap) / 4;
    for (int i = 0; i < 8; i++) {
        D2D1_RECT_F r = RF(c.left + (i % 2) * (bw + gap), c.top + (i / 2) * (bh + gap), bw, bh);
        AddHit(r, H_QA, i);
        bool rec = i == 6 && g_rec.Active();
        RRect(r, 14, Hot(H_QA, i) ? kCardHot : kCard);
        float icx = r.left + 22, icy = (r.top + r.bottom) / 2;
        D2D1_COLOR_F ic = rec ? Col(1.f, 0.35f, 0.35f) : kWhite;
        switch (i) {
            case 0: DrawToolIcon(TM_SHOT_AREA, icx, icy, ic); break;
            case 1: DrawToolIcon(TM_SHOT_FULL, icx, icy, ic); break;
            case 2: DrawToolIcon(TM_OCR, icx, icy, ic); break;
            case 3: DrawToolIcon(TM_QR, icx, icy, ic); break;
            case 4: DrawToolIcon(TM_PICK, icx, icy, ic); break;
            case 5: IconPlainText(icx, icy, ic); break;
            case 6: DrawToolIcon(TM_REC, icx, icy, ic); break;
            case 7: TabIcon(TAB_CLIP, icx, icy, ic); break;
        }
        Text(g_fQa, rec ? L"Стоп запись" : kQaNames[i], RF(r.left + 40, r.top, bw - 44, bh), DWRITE_TEXT_ALIGNMENT_LEADING, ic);
    }
}
static void DrawLauncher(D2D1_RECT_F c) {
    RRect(c, 20, kCard);
    float x = c.left + 14, cy = (c.top + c.bottom) / 2;
    const float S = 44, STEP = 58;
    int maxN = (int)((c.right - c.left - 28 - S) / STEP);
    for (int i = 0; i < (int)g_launch.size() && i < maxN; i++) {
        LaunchItem& it = g_launch[i];
        D2D1_RECT_F r = RF(x, cy - S / 2 - 2, S + 4, S + 4);
        AddHit(r, H_LAUNCH, i);
        if (Hot(H_LAUNCH, i)) RRect(r, 12, kCardHot);
        if (!it.icon.brush && !it.px.empty()) MakePic(it.icon, it.px, it.iw, it.ih);
        if (it.icon.brush) DrawPicRounded(it.icon, RF(x + 4, cy - S / 2 + 2, S - 4, S - 4), 6, 1.f);
        else Text(g_fToast, it.name.substr(0, 1), r, DWRITE_TEXT_ALIGNMENT_CENTER, kWhite);
        x += STEP;
    }
    D2D1_RECT_F add = RF(x, cy - S / 2, S, S);
    AddHit(add, H_LAUNCHADD);
    D2D1_ROUNDED_RECT rr = {add, 12, 12};
    ID2D1StrokeStyle* dash = nullptr;
    D2D1_STROKE_STYLE_PROPERTIES sp = {D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND, 10.f, D2D1_DASH_STYLE_DASH, 0.f};
    g_d2f->CreateStrokeStyle(&sp, nullptr, 0, &dash);
    SetColor(Ov(Hot(H_LAUNCHADD) ? 0.5f : 0.25f));
    g_rt->DrawRoundedRectangle(&rr, g_brush, 1.3f, dash);
    SafeRelease(dash);
    Line(add.left + S / 2 - 7, cy, add.left + S / 2 + 7, cy, 1.6f, kGray);
    Line(add.left + S / 2, cy - 7, add.left + S / 2, cy + 7, 1.6f, kGray);
    if (g_launch.empty())
        Text(g_fSmall, L"Нажми «+» и найди программу по названию — или перетащи сюда ярлык", RF(add.right + 14, c.top, c.right - add.right - 24, c.bottom - c.top),
             DWRITE_TEXT_ALIGNMENT_LEADING, WithA(kGray, 0.8f));
}
static void DrawAppPicker(D2D1_RECT_F c);  // apppick.inc
static void DrawRecTab(D2D1_RECT_F c);     // rec.inc
static void DrawHomeTab(Layout& L, D2D1_RECT_F c) {
    if (g_appPick) { DrawAppPicker(c); return; }
    float cw = c.right - c.left;
    float lw = std::floor(cw * 0.56f);
    DrawPlayerCard(L, RF(c.left, c.top, lw, 196));
    DrawVolumeCard(RF(c.left, c.top + 206, lw, 54));
    DrawQuickActions(RF(c.left + lw + 12, c.top, cw - lw - 12, 260));
    DrawLauncher(RF(c.left, c.top + 272, cw, c.bottom - c.top - 272));
}

// ---- Полка ----
static void DrawShelfTab(D2D1_RECT_F c) {
    float cw = c.right - c.left;
    if (g_shelf.empty()) {
        RRect(c, 22, kCard);
        IconTray((c.left + c.right) / 2, c.top + (c.bottom - c.top) / 2 - 24, kGray);
        Text(g_fTitle, L"Полка пустая", RF(c.left, c.top + (c.bottom - c.top) / 2, cw, 26), DWRITE_TEXT_ALIGNMENT_CENTER, kWhite);
        Text(g_fSmall, L"Перетащи файлы на остров — они подождут здесь, пока не утащишь их в нужное окно",
             RF(c.left + 40, c.top + (c.bottom - c.top) / 2 + 28, cw - 80, 18), DWRITE_TEXT_ALIGNMENT_CENTER, kGray);
        return;
    }
    wchar_t b[64];
    swprintf(b, 64, L"Перетащить все (%d)", (int)g_shelf.size());
    float aw = MeasureW(g_fToast, b) + 46;
    D2D1_RECT_F all = RF(c.left, c.top, aw, 34);
    AddHit(all, H_SHELFALL);
    RRect(all, 12, Hot(H_SHELFALL) ? kCardHot : kCard);
    for (int i = 0; i < 3; i++) for (int j = 0; j < 2; j++) Circle(all.left + 15 + j * 5, all.top + 11 + i * 6, 1.3f, kGray);
    Text(g_fToast, b, RF(all.left + 30, all.top, aw - 34, 34), DWRITE_TEXT_ALIGNMENT_LEADING, kWhite);
    D2D1_RECT_F clr = RF(c.right - 110, c.top, 110, 34);
    AddHit(clr, H_SHELFCLEAR);
    Text(g_fSmall, L"Очистить", clr, DWRITE_TEXT_ALIGNMENT_TRAILING, Hot(H_SHELFCLEAR) ? kWhite : kGray);
    const float TW = 104, TH = 118;
    int cols = std::max(1, (int)(cw / TW));
    for (int i = 0; i < (int)g_shelf.size(); i++) {
        float x = c.left + (i % cols) * TW, y = c.top + 48 + (i / cols) * TH;
        if (y + TH > c.bottom + 4) break;
        ShelfItem& it = g_shelf[i];
        D2D1_RECT_F r = RF(x + 4, y, TW - 8, TH - 8);
        AddHit(r, H_SHELFTILE, i);
        bool hot = Hot(H_SHELFTILE, i) || Hot(H_SHELFX, i);
        if (hot) RRect(r, 14, kCardHot);
        if (!it.icon.brush && !it.px.empty()) MakePic(it.icon, it.px, it.iw, it.ih);
        D2D1_RECT_F ir = RF(x + TW / 2 - 28, y + 10, 56, 56);
        if (it.icon.brush) DrawPicRounded(it.icon, ir, 8, 1.f);
        else RRect(ir, 10, Ov(0.1f));
        Text(g_fSmall, it.name, RF(x + 8, y + 72, TW - 16, 16), DWRITE_TEXT_ALIGNMENT_CENTER, kWhite);
        WIN32_FILE_ATTRIBUTE_DATA fa;
        if (GetFileAttributesExW(it.path.c_str(), GetFileExInfoStandard, &fa) && !(fa.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            Text(g_fSmall, FmtBytes((double)(((long long)fa.nFileSizeHigh << 32) | fa.nFileSizeLow)), RF(x + 8, y + 89, TW - 16, 16),
                 DWRITE_TEXT_ALIGNMENT_CENTER, WithA(kGray, 0.8f));
        if (hot) {
            D2D1_RECT_F xr = RF(r.right - 24, r.top + 4, 20, 20);
            AddHit(xr, H_SHELFX, i);
            Circle(xr.left + 10, xr.top + 10, 10, Hot(H_SHELFX, i) ? Col(1, 0.35f, 0.35f) : Col(0.25f, 0.25f, 0.3f));
            IconCross(xr.left + 10, xr.top + 10, kWhite);
        }
    }
}

// ---- Текст ----
static void DrawTextTab(D2D1_RECT_F c) {
    float cw = c.right - c.left, lw = std::floor(cw * 0.62f);
    D2D1_RECT_F ed = RF(c.left, c.top, lw, c.bottom - c.top - 32);
    RRect(ed, 20, kEdCol);
    AddHit(ed, H_EDFOCUS, 2);
    g_rcText = RF(ed.left + 14, ed.top + 12, lw - 28, ed.bottom - ed.top - 24);
    bool terr = !g_textErr.empty() && NowSec() < g_textErrUntil;
    Text(g_fSmall, terr ? g_textErr : g_textStats, RF(c.left + 6, ed.bottom + 8, lw - 120, 20), DWRITE_TEXT_ALIGNMENT_LEADING,
         terr ? Col(1.f, 0.75f, 0.4f) : kGray);
    D2D1_RECT_F bp = RF(c.left + lw - 100, ed.bottom + 4, 30, 26), bc = RF(c.left + lw - 66, ed.bottom + 4, 30, 26), bx = RF(c.left + lw - 32, ed.bottom + 4, 30, 26);
    AddHit(bp, H_TT_PASTE); AddHit(bc, H_TT_COPY); AddHit(bx, H_TT_CLEAR);
    if (Hot(H_TT_PASTE)) RRect(bp, 8, kCardHot);
    if (Hot(H_TT_COPY)) RRect(bc, 8, kCardHot);
    if (Hot(H_TT_CLEAR)) RRect(bx, 8, kCardHot);
    TabIcon(TAB_CLIP, bp.left + 15, bp.top + 13, kGray);
    StrokeRRect(RF(bc.left + 8, bc.top + 8, 10, 11), 2, 1.4f, kGray);
    StrokeRRect(RF(bc.left + 12, bc.top + 5, 10, 11), 2, 1.4f, kGray);
    IconCross(bx.left + 15, bx.top + 13, kGray);
    // инструменты
    D2D1_RECT_F tr = RF(c.left + lw + 14, c.top, cw - lw - 14, c.bottom - c.top);
    RRect(RF(tr.left, tr.top, tr.right - tr.left, 40), 14, kCard);
    RRect(RF(tr.left + 4, tr.top + 4, tr.right - tr.left - 8, 32), 11, Ov(0.1f));
    Text(g_fToast, L"Инструменты", RF(tr.left, tr.top, tr.right - tr.left, 40), DWRITE_TEXT_ALIGNMENT_CENTER, kWhite);
    D2D1_RECT_F list = RF(tr.left, tr.top + 50, tr.right - tr.left, tr.bottom - tr.top - 50);
    AddHit(list, H_TTSCROLL);
    g_rt->PushAxisAlignedClip(&list, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    float gap = 8, bw = (list.right - list.left - gap) / 2, bh = 42;
    int rows = (TT_COUNT + 1) / 2;
    float maxScroll = std::max(0.f, rows * (bh + gap) - gap - (list.bottom - list.top));
    g_ttScroll = (int)Clamp((float)g_ttScroll, 0.f, maxScroll);
    for (int i = 0; i < TT_COUNT; i++) {
        D2D1_RECT_F r = RF(list.left + (i % 2) * (bw + gap), list.top + (i / 2) * (bh + gap) - g_ttScroll, bw, bh);
        if (r.bottom < list.top || r.top > list.bottom) continue;
        AddHit(D2D1_RECT_F{r.left, std::max(r.top, list.top), r.right, std::min(r.bottom, list.bottom)}, H_TTOOL, i);
        RRect(r, 12, Hot(H_TTOOL, i) ? kCardHot : kCard);
        Text(g_fToast, TextOpName(i), r, DWRITE_TEXT_ALIGNMENT_CENTER, kWhite);
    }
    g_rt->PopAxisAlignedClip();
}

// ---- Заметка: редактор + таймер + напоминания ----
static void DrawNoteTab(D2D1_RECT_F c) {
    float cw = c.right - c.left, lw = std::floor(cw * 0.6f);
    D2D1_RECT_F ed = RF(c.left, c.top, lw, c.bottom - c.top);
    RRect(ed, 20, kEdCol);
    AddHit(RF(ed.left, ed.top, ed.right - ed.left, ed.bottom - ed.top - 46), H_EDFOCUS, 3);
    g_rcNote = RF(ed.left + 14, ed.top + 12, lw - 28, ed.bottom - ed.top - 58);
    // панель форматирования
    static const wchar_t* fmt[5] = {L"B", L"I", L"S", L"A", L"•"};
    for (int i = 0; i < 5; i++) {
        D2D1_RECT_F r = RF(ed.left + 12 + i * 36, ed.bottom - 40, 30, 28);
        AddHit(r, H_NOTEFMT, i);
        RRect(r, 8, Hot(H_NOTEFMT, i) ? kCardHot : Ov(0.06f));
        D2D1_COLOR_F col = i == 3 ? Col(1.f, 0.85f, 0.3f) : kWhite;
        Text(i == 0 ? g_fToast : g_fClip, fmt[i], r, DWRITE_TEXT_ALIGNMENT_CENTER, col);
        if (i == 2) Line(r.left + 9, r.top + 14, r.right - 9, r.top + 14, 1.3f, kWhite);
    }
    Text(g_fSmall, L"сохраняется сама", RF(ed.right - 140, ed.bottom - 40, 126, 28), DWRITE_TEXT_ALIGNMENT_TRAILING, WithA(kGray, 0.7f));
    // таймер
    float rx = c.left + lw + 14, rw = cw - lw - 14;
    D2D1_RECT_F tc = RF(rx, c.top, rw, 128);
    RRect(tc, 20, kCard);
    float rcx = tc.left + 58, rcy = tc.top + 64;
    if (g_tm.on) {
        TimerRing(rcx, rcy, 40, 5);
        Text(g_fToast, MMSS(TimerLeft()), RF(rcx - 40, rcy - 12, 80, 24), DWRITE_TEXT_ALIGNMENT_CENTER, kWhite);
    } else {
        StrokeArc(rcx, rcy, 40, 0.f, 6.2f, 5, Ov(0.12f));
        Text(g_fToast, L"Таймер", RF(rcx - 40, rcy - 12, 80, 24), DWRITE_TEXT_ALIGNMENT_CENTER, kGray);
    }
    float bx = tc.left + 116, bw = tc.right - 12 - bx;
    if (g_tm.on) {
        D2D1_RECT_F p = RF(bx, tc.top + 18, bw, 34), pl = RF(bx, tc.top + 60, (bw - 6) / 2, 30), rs = RF(bx + (bw + 6) / 2, tc.top + 60, (bw - 6) / 2, 30);
        AddHit(p, H_TPAUSE); AddHit(pl, H_TPLUS); AddHit(rs, H_TRESET);
        RRect(p, 12, Hot(H_TPAUSE) ? kCardHot : Ov(0.09f));
        Text(g_fToast, g_tm.paused ? L"▶  Дальше" : L"❚❚  Пауза", p, DWRITE_TEXT_ALIGNMENT_CENTER, kWhite);
        RRect(pl, 10, Hot(H_TPLUS) ? kCardHot : Ov(0.06f));
        Text(g_fSmall, L"+1 мин", pl, DWRITE_TEXT_ALIGNMENT_CENTER, kWhite);
        RRect(rs, 10, Hot(H_TRESET) ? kCardHot : Ov(0.06f));
        Text(g_fSmall, L"Сброс", rs, DWRITE_TEXT_ALIGNMENT_CENTER, kWhite);
        Text(g_fSmall, std::wstring(TimerLabel()) + L" · всего " + std::to_wstring((int)(g_tm.total / 60)) + L" мин",
             RF(bx, tc.top + 96, bw, 18), DWRITE_TEXT_ALIGNMENT_LEADING, kGray);
    } else {
        static const wchar_t* pre[4] = {L"5 мин", L"10 мин", L"25 мин", L"🍅 Помодоро"};
        float pw = (bw - 6) / 2;
        for (int i = 0; i < 4; i++) {
            D2D1_RECT_F r = RF(bx + (i % 2) * (pw + 6), tc.top + 22 + (i / 2) * 44, pw, 36);
            AddHit(r, H_TPRESET, i);
            RRect(r, 12, Hot(H_TPRESET, i) ? kCardHot : Ov(0.08f));
            Text(g_fSmall, pre[i], r, DWRITE_TEXT_ALIGNMENT_CENTER, kWhite);
        }
    }
    // напоминания
    D2D1_RECT_F rc = RF(rx, c.top + 138, rw, c.bottom - c.top - 138);
    RRect(rc, 20, kCard);
    IconBell(rc.left + 22, rc.top + 20, kGray);
    Text(g_fToast, L"Напоминания", RF(rc.left + 38, rc.top + 6, 140, 28), DWRITE_TEXT_ALIGNMENT_LEADING, kWhite);
    if (!g_rem.empty()) {
        float bwid = MeasureW(g_fSmall, std::to_wstring(g_rem.size())) + 14;
        RRect(RF(rc.left + 38 + MeasureW(g_fToast, L"Напоминания") + 8, rc.top + 12, bwid, 18), 9, Ov(0.12f));
        Text(g_fSmall, std::to_wstring(g_rem.size()), RF(rc.left + 38 + MeasureW(g_fToast, L"Напоминания") + 8, rc.top + 12, bwid, 18),
             DWRITE_TEXT_ALIGNMENT_CENTER, kWhite);
    }
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    long long now = ((long long)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    float y = rc.top + 40;
    float inputTop = rc.bottom - 46;
    for (int i = 0; i < (int)g_rem.size() && y + 40 <= inputTop; i++) {
        D2D1_RECT_F r = RF(rc.left + 10, y, rw - 20, 38);
        bool hot = Hot(H_REMDEL, i);
        Text(g_fClip, g_rem[i].text, RF(r.left + 6, r.top, rw - 60, 20), DWRITE_TEXT_ALIGNMENT_LEADING, kWhite);
        Text(g_fSmall, (g_rem[i].daily ? L"каждый день · " : L"") + ReminderWhen(g_rem[i].due, now), RF(r.left + 6, r.top + 19, rw - 60, 16),
             DWRITE_TEXT_ALIGNMENT_LEADING, kGray);
        D2D1_RECT_F xr = RF(r.right - 26, r.top + 8, 22, 22);
        AddHit(xr, H_REMDEL, i);
        if (hot) Circle(xr.left + 11, xr.top + 11, 11, Ov(0.1f));
        IconCross(xr.left + 11, xr.top + 11, hot ? kWhite : WithA(kGray, 0.7f));
        y += 42;
    }
    if (g_rem.empty())
        Text(g_fSmall, L"«позвонить маме завтра в 9», «через 20 минут плита», «таблетки каждый день в 23:00»",
             RF(rc.left + 16, rc.top + 42, rw - 32, 34), DWRITE_TEXT_ALIGNMENT_LEADING, WithA(kGray, 0.8f));
    if (!g_remErr.empty() && NowSec() < g_remErrUntil)
        Text(g_fSmall, g_remErr, RF(rc.left + 16, inputTop - 20, rw - 32, 18), DWRITE_TEXT_ALIGNMENT_LEADING, Col(1.f, 0.6f, 0.45f));
    D2D1_RECT_F in = RF(rc.left + 10, inputTop, rw - 20, 36);
    DrawIEdit(IE_REMIND, in, L"Напомни… (Enter — добавить)", 12);
}

// ---- Погода ----
static D2D1_COLOR_F SkyTop() {
    int ic = WeatherIcon(g_wx.code);
    if (!g_wx.isDay) return Col(0.10f, 0.12f, 0.28f);
    if (ic == WX_CLEAR || ic == WX_PARTLY) return Col(0.25f, 0.55f, 0.95f);
    if (ic == WX_RAIN || ic == WX_DRIZZLE || ic == WX_THUNDER) return Col(0.30f, 0.36f, 0.48f);
    if (ic == WX_SNOW) return Col(0.55f, 0.65f, 0.80f);
    return Col(0.42f, 0.50f, 0.62f);
}
static void DrawWeatherTab(D2D1_RECT_F c) {
    if (!g_wx.ok) {
        RRect(c, 22, kCard);
        Text(g_fTitle, g_wx.error.empty() ? L"Загружаю погоду…" : g_wx.error, RF(c.left, c.top, c.right - c.left, c.bottom - c.top - 20),
             DWRITE_TEXT_ALIGNMENT_CENTER, kWhite);
        Text(g_fSmall, L"Город задаётся в настройках", RF(c.left, (c.top + c.bottom) / 2 + 6, c.right - c.left, 18), DWRITE_TEXT_ALIGNMENT_CENTER, kGray);
        return;
    }
    float cw = c.right - c.left;
    // карточка «сейчас» на фоне неба
    D2D1_RECT_F now = RF(c.left, c.top, 236, c.bottom - c.top);
    ID2D1GradientStopCollection* col = nullptr;
    ID2D1LinearGradientBrush* gb = nullptr;
    D2D1_COLOR_F top = SkyTop(), bot = Lerp(top, Col(0.05f, 0.06f, 0.1f), 0.55f);
    D2D1_GRADIENT_STOP gs[2] = {{0.f, top}, {1.f, bot}};
    g_rt->CreateGradientStopCollection(gs, 2, D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP, &col);
    D2D1_LINEAR_GRADIENT_BRUSH_PROPERTIES lg = {Pt(now.left, now.top), Pt(now.left, now.bottom)};
    if (col) g_rt->CreateLinearGradientBrush(&lg, nullptr, col, &gb);
    D2D1_ROUNDED_RECT rr = {now, 20, 20};
    if (gb) g_rt->FillRoundedRectangle(&rr, gb);
    SafeRelease(gb);
    SafeRelease(col);
    D2D1_RECT_F ref = RF(now.right - 36, now.top + 10, 26, 26);
    AddHit(ref, H_WXREFRESH);
    IconRestart(ref.left + 13, ref.top + 13, Sky(Hot(H_WXREFRESH) ? 1.f : 0.7f));
    Text(g_fToast, g_wx.city.empty() ? L"Погода" : g_wx.city, RF(now.left + 16, now.top + 10, 190, 26), DWRITE_TEXT_ALIGNMENT_LEADING, kWhite);
    DrawWx(WeatherIcon(g_wx.code), g_wx.isDay, now.left + 46, now.top + 78, 2.2f, top);
    Text(g_fBig, Temp(g_wx.temp), RF(now.left + 90, now.top + 44, 140, 64), DWRITE_TEXT_ALIGNMENT_LEADING, kWhite);
    Text(g_fToast, WeatherText(g_wx.code), RF(now.left + 16, now.top + 118, 210, 24), DWRITE_TEXT_ALIGNMENT_LEADING, kWhite);
    Text(g_fSmall, L"Ощущается как " + Temp(g_wx.feels) + L" · " + Temp(g_wx.tmin[0]) + L"…" + Temp(g_wx.tmax[0]),
         RF(now.left + 16, now.top + 142, 210, 18), DWRITE_TEXT_ALIGNMENT_LEADING, Sky(0.8f));
    wchar_t chip[3][32];
    swprintf(chip[0], 32, L"%.0f м/с", g_wx.wind);
    swprintf(chip[1], 32, L"%.0f%%", g_wx.humidity);
    swprintf(chip[2], 32, L"%.1f мм", g_wx.precip);
    static const wchar_t* chipT[3] = {L"ветер", L"влажность", L"осадки"};
    float chw = (now.right - now.left - 32 - 12) / 3;
    for (int i = 0; i < 3; i++) {
        D2D1_RECT_F r = RF(now.left + 16 + i * (chw + 6), now.bottom - 86, chw, 52);
        RRect(r, 12, Sky(0.14f));
        Text(g_fSmall, chipT[i], RF(r.left, r.top + 6, chw, 16), DWRITE_TEXT_ALIGNMENT_CENTER, Sky(0.75f));
        Text(g_fToast, chip[i], RF(r.left, r.top + 22, chw, 24), DWRITE_TEXT_ALIGNMENT_CENTER, kWhite);
    }
    if (!g_wx.sunrise.empty())
        Text(g_fSmall, L"восход " + g_wx.sunrise + L"   закат " + g_wx.sunset, RF(now.left + 16, now.bottom - 28, 210, 18),
             DWRITE_TEXT_ALIGNMENT_LEADING, Sky(0.75f));
    // по часам
    float rx = now.right + 12, rw = c.right - rx;
    D2D1_RECT_F hc = RF(rx, c.top, rw, 104);
    RRect(hc, 20, kCard);
    int nh = std::min(g_wx.hours, 9);
    float colW = (rw - 20) / std::max(1, nh);
    for (int i = 0; i < nh; i++) {
        float x = hc.left + 10 + i * colW;
        wchar_t h[16];
        swprintf(h, 16, L"%d:00", g_wx.hhour[i]);
        Text(g_fSmall, i == 0 ? std::wstring(L"Сейчас") : std::wstring(h), RF(x, hc.top + 10, colW, 16), DWRITE_TEXT_ALIGNMENT_CENTER, kGray);
        DrawWx(WeatherIcon(g_wx.hcode[i]), g_wx.hday[i], x + colW / 2, hc.top + 48, 1.0f, kBgSolid);
        Text(g_fToast, Temp(g_wx.htemp[i]), RF(x, hc.top + 70, colW, 22), DWRITE_TEXT_ALIGNMENT_CENTER, kWhite);
    }
    // неделя
    D2D1_RECT_F wc = RF(rx, c.top + 114, rw, c.bottom - c.top - 114);
    RRect(wc, 20, kCard);
    double lo = 1e9, hi = -1e9;
    for (int i = 0; i < g_wx.days; i++) { lo = std::min(lo, g_wx.tmin[i]); hi = std::max(hi, g_wx.tmax[i]); }
    if (hi - lo < 1) hi = lo + 1;
    static const wchar_t* wd[7] = {L"Вс", L"Пн", L"Вт", L"Ср", L"Чт", L"Пт", L"Сб"};
    float rowH = (wc.bottom - wc.top - 16) / std::max(1, g_wx.days);
    for (int i = 0; i < g_wx.days; i++) {
        float y = wc.top + 8 + i * rowH, cy = y + rowH / 2;
        Text(g_fClip, i == 0 ? L"Сегодня" : wd[g_wx.dweekday[i] % 7], RF(wc.left + 16, y, 80, rowH), DWRITE_TEXT_ALIGNMENT_LEADING, kWhite);
        DrawWx(WeatherIcon(g_wx.dcode[i]), true, wc.left + 108, cy, 0.8f, kBgSolid);
        float bx0 = wc.left + 176, bx1 = wc.right - 130;
        Text(g_fSmall, Temp(g_wx.tmin[i]), RF(bx0 - 44, y, 38, rowH), DWRITE_TEXT_ALIGNMENT_TRAILING, kGray);
        RRect(RF(bx0, cy - 2.5f, bx1 - bx0, 5), 2.5f, Ov(0.08f));
        float a = (float)((g_wx.tmin[i] - lo) / (hi - lo)), b = (float)((g_wx.tmax[i] - lo) / (hi - lo));
        ID2D1GradientStopCollection* gc = nullptr;
        ID2D1LinearGradientBrush* gbr = nullptr;
        D2D1_GRADIENT_STOP st2[2] = {{0.f, Col(0.45f, 0.75f, 1.f)}, {1.f, Col(1.f, 0.72f, 0.3f)}};
        g_rt->CreateGradientStopCollection(st2, 2, D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP, &gc);
        D2D1_LINEAR_GRADIENT_BRUSH_PROPERTIES lb = {Pt(bx0, 0), Pt(bx1, 0)};
        if (gc) g_rt->CreateLinearGradientBrush(&lb, nullptr, gc, &gbr);
        D2D1_ROUNDED_RECT seg = {RF(bx0 + (bx1 - bx0) * a, cy - 2.5f, std::max(5.f, (bx1 - bx0) * (b - a)), 5), 2.5f, 2.5f};
        if (gbr) g_rt->FillRoundedRectangle(&seg, gbr);
        SafeRelease(gbr);
        SafeRelease(gc);
        Text(g_fToast, Temp(g_wx.tmax[i]), RF(bx1 + 6, y, 44, rowH), DWRITE_TEXT_ALIGNMENT_LEADING, kWhite);
        if (g_wx.dprecip[i] >= 0.1) {
            wchar_t p[16];
            swprintf(p, 16, L"%.1f мм", g_wx.dprecip[i]);
            Text(g_fSmall, p, RF(wc.right - 76, y, 60, rowH), DWRITE_TEXT_ALIGNMENT_TRAILING, Col(0.5f, 0.75f, 1.f));
        }
    }
    Text(g_fSmall, L"Данные Open-Meteo", RF(wc.right - 160, wc.bottom - 4, 150, 14), DWRITE_TEXT_ALIGNMENT_TRAILING, WithA(kGray, 0.5f));
}

// ---- Управление ----
static void DrawCtrlTab(D2D1_RECT_F c) {
    float cw = c.right - c.left, gap = 12, tw4 = (cw - 3 * gap) / 4, th = 112;
    const D2D1_COLOR_F on = g_accent;
    SYSTEM_POWER_STATUS ps;
    bool hasBat = GetSystemPowerStatus(&ps) && ps.BatteryFlag != 128 && ps.BatteryFlag != 255;
    static const int order[6] = {0, 1, 5, 2, 4, 3};
    float tw5 = (cw - 5 * gap) / 6;
    for (int k = 0; k < 6; k++) {
        int i = order[k];
        D2D1_RECT_F r = RF(c.left + k * (tw5 + gap), c.top, tw5, th);
        bool clickable = i != 3;
        if (clickable) AddHit(r, H_CTRL, i);
        RRect(r, 20, Hot(H_CTRL, i) ? Ov(0.09f) : kCard);
        bool st = false;
        std::wstring title, sub;
        if (i == 0) {
            st = g_wifiState == 1;
            title = L"Wi-Fi";
            sub = g_wifiState == -2 ? L"…" : g_wifiState < 0 ? L"нет адаптера" : st ? (g_wifiName.empty() ? L"Включён" : g_wifiName) : L"Выключен";
            if (g_wifiState == -1 || (g_wifiState == 0 && g_ethUp)) { title = L"Интернет"; st = g_ethUp; sub = g_ethUp ? L"По кабелю" : L"Нет сети"; }
        }
        if (i == 1) { st = g_btState == 1; title = L"Bluetooth"; sub = g_btState == -2 ? L"…" : g_btState < 0 ? L"нет адаптера" : st ? L"Включён" : L"Выключен"; }
        if (i == 2) { st = g_darkTheme; title = L"Тёмная тема"; sub = st ? L"Включена" : L"Выключена"; }
        if (i == 3) {
            title = L"Батарея";
            if (hasBat) { sub = ps.ACLineStatus == 1 ? L"Заряжается" : L"Работа от батареи"; }
            else { title = L"Питание"; sub = L"От сети"; }
        }
        if (i == 4) { st = g_set.dnd; title = L"Не беспокоить"; sub = st ? L"Включено" : L"Выключено"; }
        if (i == 5) {
            st = g_vpn.on;
            title = L"VPN";
            std::wstring where = !g_vpn.country.empty() ? g_vpn.country : g_vpn.cc;  // страна, а не длинный IPv6
            sub = !g_vpnInit ? L"…" : (g_vpn.on ? (where.empty() ? (g_vpn.adapter.empty() ? L"Включён" : g_vpn.adapter) : where) : L"Выключен");
        }
        float icx = r.left + 32, icy = r.top + 32;
        if (i != 3) Circle(icx, icy, 18, st ? on : Ov(0.1f));
        D2D1_COLOR_F ic = st ? Col(1, 1, 1) : kWhite;
        if (i == 4) IconMoon(icx, icy, ic, st ? on : Lerp(kBgSolid, kWhite, 0.1f));
        if (i == 5) IconShield(icx, icy, ic, false);
        if (i == 0) IconWifi(icx, icy, ic);
        if (i == 1) IconBt(icx, icy, ic);
        if (i == 2) {  // полумесяц-«инь»: левая половина залита, контур целиком
            Ring(icx, icy, 7.f, 1.6f, ic);
            ID2D1PathGeometry* g = nullptr;
            ID2D1GeometrySink* sk = nullptr;
            if (SUCCEEDED(g_d2f->CreatePathGeometry(&g)) && SUCCEEDED(g->Open(&sk))) {
                sk->BeginFigure(Pt(icx, icy - 7.f), D2D1_FIGURE_BEGIN_FILLED);
                D2D1_ARC_SEGMENT a = {Pt(icx, icy + 7.f), {7.f, 7.f}, 0.f, D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE, D2D1_ARC_SIZE_SMALL};
                sk->AddArc(&a);
                sk->EndFigure(D2D1_FIGURE_END_CLOSED);
                sk->Close();
                sk->Release();
                SetColor(ic);
                g_rt->FillGeometry(g, g_brush);
            }
            SafeRelease(g);
        }
        if (i == 3) {
            int pct = hasBat ? ps.BatteryLifePercent : 100;
            IconBattery(r.left + 30, icy, pct / 100.f, hasBat && ps.ACLineStatus == 1, pct <= 20 ? Col(1, 0.4f, 0.3f) : kWhite);
            if (hasBat) Text(g_fToast, std::to_wstring(pct) + L"%", RF(r.left + 48, icy - 12, 70, 24), DWRITE_TEXT_ALIGNMENT_LEADING, kWhite);
        }
        IDWriteTextFormat* tf = MeasureW(g_fToast, title) > tw5 - 26 ? g_fQa : g_fToast;  // «Не беспокоить» не влезает — шрифт поменьше
        Text(tf, title, RF(r.left + 16, r.bottom - 56, tw5 - 22, 22), DWRITE_TEXT_ALIGNMENT_LEADING, kWhite);
        Text(g_fSmall, sub, RF(r.left + 16, r.bottom - 32, tw5 - 24, 18), DWRITE_TEXT_ALIGNMENT_LEADING, kGray);
    }
    static const wchar_t* pn[4] = {L"Блокировка", L"Сон", L"Перезагрузка", L"Выключение"};
    bool armed = g_powerArm >= 0 && NowSec() < g_powerArmUntil;
    for (int i = 0; i < 4; i++) {
        D2D1_RECT_F r = RF(c.left + i * (tw4 + gap), c.top + th + gap, tw4, 52);
        AddHit(r, H_POWER, i);
        bool arm = armed && g_powerArm == i;
        RRect(r, 18, arm ? Col(0.85f, 0.25f, 0.25f, 0.9f) : Hot(H_POWER, i) ? Ov(0.09f) : kCard);
        float icx = (r.left + r.right) / 2, icy = r.top + 18;
        if (i == 0) IconLock(icx, icy, kWhite);
        if (i == 1) IconSleep(icx, icy, kWhite);
        if (i == 2) IconRestart(icx, icy, kWhite);
        if (i == 3) IconPower(icx, icy, kWhite);
        Text(g_fSmall, arm ? L"Нажми ещё раз" : pn[i], RF(r.left, r.top + 30, tw4, 20), DWRITE_TEXT_ALIGNMENT_CENTER, kWhite);
    }
    // кто сейчас слушает микрофон и смотрит в камеру
    float py = c.top + th + gap + 52 + gap, hw = (cw - gap) / 2;
    for (int k = 0; k < 2; k++) {
        D2D1_RECT_F r = RF(c.left + k * (hw + gap), py, hw, 34);
        bool act = k == 0 ? g_priv.mic : g_priv.cam;
        const std::wstring& app = k == 0 ? g_priv.micApp : g_priv.camApp;
        D2D1_COLOR_F dc = k == 0 ? Col(1.f, 0.62f, 0.12f) : Col(0.20f, 0.84f, 0.36f);
        RRect(r, 14, act ? WithA(dc, 0.14f) : kCard);
        Circle(r.left + 18, py + 17, 4.f, act ? dc : Ov(0.25f));
        std::wstring t = std::wstring(k == 0 ? L"Микрофон" : L"Камера") + L" · " + (act ? (app.empty() ? L"используется" : app) : L"никто не использует");
        Text(g_fSmall, t, RF(r.left + 32, r.top, hw - 40, 34), DWRITE_TEXT_ALIGNMENT_LEADING, act ? kWhite : kGray);
    }
    float sy = py + 34 + gap;
    DrawStatsRow(RF(c.left, sy, cw, c.bottom - sy));
}

static void DrawExpandedPanel(Layout& L, float ox, float oy, float tw, float th) {
    g_hits.clear();
    g_rcText = g_rcNote = g_rcRemind = D2D1_RECT_F{};
    DrawTabBar(ox, oy, tw);
    D2D1_RECT_F c = RF(ox + 22, oy + 68, tw - 44, th - 68 - 22);
    if (g_tab != TAB_SETTINGS && (g_set.tabHidden & (1 << g_tab)))
        for (int pos = 0; pos < TAB_COUNT; pos++) {
            int t = g_set.tabOrder[pos] - L'0';
            if (!(g_set.tabHidden & (1 << t))) { g_tab = t; break; }
        }
    int tab = g_mode == M_CLIP ? TAB_CLIP : g_tab;
    // плавный въезд вкладки
    float slide = (float)sTab.x;
    bool sliding = std::fabs(slide) > 0.002f;
    D2D1_MATRIX_3X2_F prevT;
    g_rt->GetTransform(&prevT);
    if (sliding) {
        D2D1_LAYER_PARAMETERS lp = {};
        lp.contentBounds = RF(ox, c.top - 6, tw, th);
        lp.maskAntialiasMode = D2D1_ANTIALIAS_MODE_PER_PRIMITIVE;
        lp.maskTransform = D2D1::Matrix3x2F::Identity();
        lp.opacity = 1.f - std::min(1.f, std::fabs(slide)) * 0.85f;
        g_rt->PushLayer(&lp, g_layer3);
        g_rt->SetTransform(D2D1::Matrix3x2F::Translation(slide * 46.f, 0) * prevT);
    }
    switch (tab) {
        case TAB_HOME: DrawHomeTab(L, c); break;
        case TAB_CLIP: DrawClipPanel(c.left - 14, c.top - 14, c.right - c.left + 28, c.bottom - c.top + 14); break;
        case TAB_REC: DrawRecTab(c); break;
        case TAB_TEXT: DrawTextTab(c); break;
        case TAB_NOTE: DrawNoteTab(c); break;
        case TAB_WEATHER: DrawWeatherTab(c); break;
        case TAB_CTRL: DrawCtrlTab(c); break;
        case TAB_SETTINGS: DrawSettingsTab(c); break;
    }
    if (sliding) {
        g_rt->SetTransform(prevT);
        g_rt->PopLayer();
    }
}
