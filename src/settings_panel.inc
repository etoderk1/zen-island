// settings_panel.inc — настройки прямо в острове (вместо отдельного окна).
// Подключается в island.cpp после panel_logic.inc.
// ----------------------------------------------------------------------------

static void ApplySettings();
static void ApplyTheme();
static void OnMedia();
static void MediaFilterReset();

enum { S_NONE, S_APPLY, S_THEME, S_AUTOSTART, S_CLIPCLEAR, S_RESET, S_MONITOR, S_TABS, S_OPENDATA, S_CLIPLIMIT, S_HOOKS, S_EXPORT, S_IMPORT, S_FX, S_PASS, S_MEDIA, S_MEDIASRC, S_VPN, S_GAME, S_ADMIN, S_ADMINTASK, S_UPDCHECK, S_WELCOME, S_BACKUP, S_RESTORE, S_LOGS };
// источники музыки, которые уже видели (для переключателей)
static std::wstring g_srcIds[24], g_srcNames[24];
static bool g_srcOn[24];
static int g_srcN = 0;
struct SCtl {
    int kind = 0;           // 0 переключатель, 1 ползунок, 2 выбор, 3 кнопка
    bool* b = nullptr;
    int* i = nullptr;
    int mn = 0, mx = 0, step = 1;
    int special = S_APPLY;
    D2D1_RECT_F track = {};
};
static std::vector<SCtl> g_sctl;
static int g_setArm = -1;
static double g_setArmUntil = 0;
static int g_monSel = 0;
static bool g_autostartUi = false, g_adminTaskUi = false;
static const wchar_t* kSecNames[] = {L"Главное", L"Вид", L"Питомец", L"Музыка", L"Уведомления", L"Игры", L"Инструменты", L"Календарь",
                                     L"Дополнительно", L"О программе"};
static const int kSecCount = 10;
static const int kSecOrder[10] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};  // порядок в списке слева
static bool g_custOpen = false;  // открыт конструктор «Свой стиль»
// стили, которые показываем (остальные доступны через конструктор)
static const int kStyleShown[9] = {ST_NOTCH, ST_MAC, ST_IPHONE, ST_GLASS, ST_AURORA, ST_WIN11, ST_MATERIAL, ST_SEASON, ST_CUSTOM};
static const D2D1_COLOR_F kSwatch[8] = {{0.56f, 0.50f, 1.0f, 1}, {0.25f, 0.60f, 1.0f, 1}, {0.20f, 0.80f, 0.75f, 1}, {0.30f, 0.82f, 0.40f, 1},
                                       {1.0f, 0.78f, 0.25f, 1},  {1.0f, 0.55f, 0.25f, 1},  {1.0f, 0.36f, 0.42f, 1},  {0.92f, 0.40f, 0.85f, 1}};

// ---- рисование строк ----
struct SRow { float x, w, y; };  // y — в координатах прокрутки
static bool RowVisible(float top, float h) { return top + h > g_setClip.top - 2 && top < g_setClip.bottom + 2; }
static void SwitchGfx(float x, float cy, bool on, bool hot) {
    D2D1_RECT_F tr = RF(x, cy - 12, 42, 24);
    RRect(tr, 12, on ? g_accent : Ov(hot ? 0.22f : 0.16f));
    Circle(on ? x + 30 : x + 12, cy, 9, on ? Col(1, 1, 1) : (g_light ? Col(1, 1, 1) : Col(0.85f, 0.85f, 0.88f)));
}
static void SHeader(SRow& r, const wchar_t* t) {
    float y = r.y - g_setScroll;
    if (RowVisible(y, 30)) Text(g_fSmall, t, RF(r.x + 4, y + 8, r.w, 18), DWRITE_TEXT_ALIGNMENT_LEADING, WithA(kGray, 0.9f));
    r.y += 30;
}
static float STitle(float x, float y, float w, float h, const std::wstring& title, const std::wstring& sub) {
    if (sub.empty()) Text(g_fClip, title, RF(x, y, w, h), DWRITE_TEXT_ALIGNMENT_LEADING, kWhite);
    else {
        Text(g_fClip, title, RF(x, y + h / 2 - 19, w, 20), DWRITE_TEXT_ALIGNMENT_LEADING, kWhite);
        Text(g_fSmall, sub, RF(x, y + h / 2 + 1, w, 17), DWRITE_TEXT_ALIGNMENT_LEADING, kGray);
    }
    return 0;
}
static void SToggle(SRow& r, const std::wstring& title, const std::wstring& sub, bool& v, int special = S_APPLY) {
    float h = sub.empty() ? 44.f : 52.f, y = r.y - g_setScroll;
    SCtl c;
    c.kind = 0; c.b = &v; c.special = special;
    int idx = (int)g_sctl.size();
    g_sctl.push_back(c);
    if (RowVisible(y, h)) {
        D2D1_RECT_F row = RF(r.x, y, r.w, h - 4);
        bool hot = Hot(H_SCTL, idx);
        if (hot) RRect(row, 12, Ov(0.04f));
        STitle(r.x + 12, y, r.w - 80, h - 4, title, sub);
        SwitchGfx(r.x + r.w - 56, y + (h - 4) / 2, v, hot);
        AddHit(row, H_SCTL, idx);
    }
    r.y += h;
}
static void SSlider(SRow& r, const std::wstring& title, int& v, int mn, int mx, int step, const std::wstring& valueText,
                    int special = S_APPLY) {
    float h = 58, y = r.y - g_setScroll;
    SCtl c;
    c.kind = 1; c.i = &v; c.mn = mn; c.mx = mx; c.step = step; c.special = special;
    int idx = (int)g_sctl.size();
    if (RowVisible(y, h)) {
        Text(g_fClip, title, RF(r.x + 12, y + 4, r.w - 120, 22), DWRITE_TEXT_ALIGNMENT_LEADING, kWhite);
        Text(g_fSmall, valueText, RF(r.x + r.w - 220, y + 4, 208, 22), DWRITE_TEXT_ALIGNMENT_TRAILING, kGray);
        float x0 = r.x + 14, x1 = r.x + r.w - 14, cy = y + 38;
        c.track = RF(x0, cy - 11, x1 - x0, 22);
        float f = mx > mn ? (float)(v - mn) / (mx - mn) : 0;
        RRect(RF(x0, cy - 3, x1 - x0, 6), 3, Ov(0.14f));
        RRect(RF(x0, cy - 3, std::max(6.f, (x1 - x0) * f), 6), 3, g_accent);
        bool hot = Hot(H_SSLIDER, idx) || g_setDrag == idx;
        Circle(x0 + (x1 - x0) * f, cy, hot ? 9.f : 8.f, Col(1, 1, 1));
        StrokeArc(x0 + (x1 - x0) * f, cy, hot ? 9.f : 8.f, 0.f, 6.2832f, 1.f, Col(0, 0, 0, 0.15f));
        AddHit(c.track, H_SSLIDER, idx);
    }
    g_sctl.push_back(c);
    r.y += h;
}
static void SChoice(SRow& r, const std::wstring& title, int& v, const std::vector<std::wstring>& opts, int special = S_APPLY) {
    float y = r.y - g_setScroll;
    SCtl c;
    c.kind = 2; c.i = &v; c.special = special;
    int idx = (int)g_sctl.size();
    g_sctl.push_back(c);
    // чипы могут уйти на вторую строку
    float cx = r.x + 12, cy = y + 30, maxX = r.x + r.w - 12;
    float rowH = 30;
    std::vector<D2D1_RECT_F> rects;
    for (size_t k = 0; k < opts.size(); k++) {
        float w = MeasureW(g_fSmall, opts[k]) + 28;
        if (cx + w > maxX && cx > r.x + 12) { cx = r.x + 12; cy += 36; rowH += 36; }
        rects.push_back(RF(cx, cy, w, 30));
        cx += w + 6;
    }
    float h = 30 + rowH + 10;
    if (RowVisible(y, h)) {
        Text(g_fClip, title, RF(r.x + 12, y + 2, r.w - 24, 22), DWRITE_TEXT_ALIGNMENT_LEADING, kWhite);
        for (size_t k = 0; k < opts.size(); k++) {
            bool sel = v == (int)k, hot = Hot(H_SCHOICE, idx * 32 + (int)k);
            RRect(rects[k], 15, sel ? g_accent : Ov(hot ? 0.14f : 0.08f));
            Text(g_fSmall, opts[k], rects[k], DWRITE_TEXT_ALIGNMENT_CENTER, sel ? Col(1, 1, 1) : kWhite);
            AddHit(rects[k], H_SCHOICE, idx * 32 + (int)k);
        }
    }
    r.y += h;
}
static void SButton(SRow& r, const std::wstring& title, const std::wstring& sub, const std::wstring& label, int special, bool danger = false) {
    float h = sub.empty() ? 46.f : 54.f, y = r.y - g_setScroll;
    SCtl c;
    c.kind = 3; c.special = special;
    int idx = (int)g_sctl.size();
    g_sctl.push_back(c);
    if (RowVisible(y, h)) {
        STitle(r.x + 12, y, r.w - 200, h - 4, title, sub);
        bool armed = danger && g_setArm == idx && NowSec() < g_setArmUntil;
        std::wstring lb = armed ? L"Точно? Нажми ещё" : label;
        float bw = MeasureW(g_fSmall, lb) + 30;
        D2D1_RECT_F b = RF(r.x + r.w - 12 - bw, y + (h - 4) / 2 - 15, bw, 30);
        RRect(b, 15, armed ? Col(0.85f, 0.25f, 0.25f) : Ov(Hot(H_SCTL, idx) ? 0.16f : 0.09f));
        Text(g_fSmall, lb, b, DWRITE_TEXT_ALIGNMENT_CENTER, armed ? Col(1, 1, 1) : kWhite);
        AddHit(b, H_SCTL, idx);
    }
    r.y += h;
}
static void SInput(SRow& r, const std::wstring& title, const std::wstring& hint, D2D1_RECT_F& edRect, int which) {
    float h = 84, y = r.y - g_setScroll;
    edRect = D2D1_RECT_F{};
    if (RowVisible(y, h)) {
        Text(g_fClip, title, RF(r.x + 12, y + 2, r.w - 24, 22), DWRITE_TEXT_ALIGNMENT_LEADING, kWhite);
        D2D1_RECT_F box = RF(r.x + 12, y + 28, r.w - 24, 34);
        DrawIEdit(which, box, which == IE_CITY ? L"Например: Иркутск" : which == IE_ICS ? L"https://…/basic.ics" : L"game.exe, obs64.exe");
        Text(g_fSmall, hint, RF(r.x + 14, y + 64, r.w - 28, 16), DWRITE_TEXT_ALIGNMENT_LEADING, WithA(kGray, 0.8f));
    }
    r.y += h;
}
static void SInfo(SRow& r, const std::wstring& t, float h = 22) {
    float y = r.y - g_setScroll;
    if (RowVisible(y, h)) Text(g_fSmall, t, RF(r.x + 12, y, r.w - 24, h), DWRITE_TEXT_ALIGNMENT_LEADING, kGray);
    r.y += h;
}

static void DrawTabsEditor(SRow& r) {
    for (int pos = 0; pos < 7; pos++) {
        int t = g_set.tabOrder[pos] - L'0';
        if (t == TAB_NOTE) continue;  // заметки пока убраны
        float y = r.y - g_setScroll, h = 46;
        if (RowVisible(y, h)) {
            bool vis = !(g_set.tabHidden & (1 << t));
            D2D1_RECT_F row = RF(r.x, y, r.w, h - 4);
            RRect(row, 12, kCard);
            TabIcon(t, r.x + 26, y + 21, vis ? kWhite : kGray);
            Text(g_fClip, kTabNames[t], RF(r.x + 48, y, 200, h - 4), DWRITE_TEXT_ALIGNMENT_LEADING, vis ? kWhite : kGray);
            D2D1_RECT_F up = RF(r.x + r.w - 150, y + 6, 30, 30), dn = RF(r.x + r.w - 116, y + 6, 30, 30);
            if (pos > 0) {
                AddHit(up, H_STABUP, pos);
                if (Hot(H_STABUP, pos)) Circle(up.left + 15, up.top + 15, 15, Ov(0.1f));
                Line(up.left + 9, up.top + 18, up.left + 15, up.top + 12, 1.8f, kGray);
                Line(up.left + 15, up.top + 12, up.left + 21, up.top + 18, 1.8f, kGray);
            }
            if (pos < 6) {
                AddHit(dn, H_STABDN, pos);
                if (Hot(H_STABDN, pos)) Circle(dn.left + 15, dn.top + 15, 15, Ov(0.1f));
                Line(dn.left + 9, dn.top + 12, dn.left + 15, dn.top + 18, 1.8f, kGray);
                Line(dn.left + 15, dn.top + 18, dn.left + 21, dn.top + 12, 1.8f, kGray);
            }
            D2D1_RECT_F sw = RF(r.x + r.w - 68, y + 6, 56, 30);
            AddHit(sw, H_STABVIS, t);
            SwitchGfx(r.x + r.w - 60, y + 21, vis, Hot(H_STABVIS, t));
        }
        r.y += h;
    }
}

// логотип Zen Island: тёмный градиентный квадрат, круг-энсо и пилюля-остров
static void DrawLogo(float x, float y, float s) {
    D2D1_RECT_F r = RF(x, y, s, s);
    D2D1_GRADIENT_STOP gs[2] = {{0.f, Col(0.15f, 0.12f, 0.34f)}, {1.f, Col(0.05f, 0.23f, 0.29f)}};
    ID2D1GradientStopCollection* col = nullptr;
    ID2D1LinearGradientBrush* gb = nullptr;
    g_rt->CreateGradientStopCollection(gs, 2, D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP, &col);
    D2D1_LINEAR_GRADIENT_BRUSH_PROPERTIES lg = {Pt(r.left, r.top), Pt(r.right, r.bottom)};
    if (col) g_rt->CreateLinearGradientBrush(&lg, nullptr, col, &gb);
    D2D1_ROUNDED_RECT rr = {r, s * 0.225f, s * 0.225f};
    if (gb) g_rt->FillRoundedRectangle(&rr, gb);
    else RRect(r, s * 0.225f, Col(0.12f, 0.15f, 0.3f));
    SafeRelease(gb);
    SafeRelease(col);
    float cx = x + s / 2, cy = y + s * 0.52f;
    StrokeArc(cx, cy, s * 0.33f, -1.05f, 4.19f, std::max(1.5f, s * 0.033f), Col(0.91f, 0.89f, 1.f, 0.85f));
    float pw = s * 0.56f, ph = s * 0.2f;
    RRect(RF(cx - pw / 2, cy - ph / 2, pw, ph), ph / 2, Col(0.02f, 0.02f, 0.04f));
    Circle(cx - pw / 2 + ph * 0.5f, cy, ph * 0.26f, Col(0.59f, 0.53f, 1.f));
    RRect(RF(cx - pw / 2 + ph * 0.95f, cy - ph * 0.09f, pw - ph * 1.45f, ph * 0.18f), ph * 0.09f, Col(0.94f, 0.94f, 0.96f));
}
static std::wstring HotkeyCmdText();
static void DrawSettingsTab(D2D1_RECT_F c) {
    g_sctl.clear();
    // слева — разделы
    D2D1_RECT_F nav = RF(c.left, c.top, 180, c.bottom - c.top);
    RRect(nav, 18, kCard);
    for (int k = 0; k < kSecCount; k++) {
        int i = kSecOrder[k];
        D2D1_RECT_F r = RF(nav.left + 8, nav.top + 6 + k * 34, 164, 31);
        AddHit(r, H_SNAV, i);
        if (g_setSec == i) RRect(r, 11, Ov(0.11f));
        else if (Hot(H_SNAV, i)) RRect(r, 11, Ov(0.05f));
        if (i == 8 || i == 9) Line(r.left + 8, r.top - 2, r.right - 8, r.top - 2, 1.f, i == 8 ? Ov(0.08f) : Ov(0.f));
        Text(g_fClip, kSecNames[i], RF(r.left + 12, r.top, r.right - r.left - 16, r.bottom - r.top), DWRITE_TEXT_ALIGNMENT_LEADING,
             g_setSec == i ? kWhite : kGray);
    }
    // справа — содержимое с прокруткой
    D2D1_RECT_F area = RF(c.left + 192, c.top, c.right - c.left - 192, c.bottom - c.top);
    if (g_setSec != 1) g_custOpen = false;
    if (g_setSec == 1 && g_custOpen) {  // конструктор: сверху закреплённое живое превью
        D2D1_RECT_F back = RF(area.left, area.top, 30, 30);
        AddHit(back, H_SCUST, 1);
        Circle(area.left + 15, area.top + 15, 15, Hot(H_SCUST, 1) ? Ov(0.14f) : kCard);
        Line(area.left + 17, area.top + 9, area.left + 11, area.top + 15, 1.8f, kWhite);
        Line(area.left + 11, area.top + 15, area.left + 17, area.top + 21, 1.8f, kWhite);
        Text(g_fTitle, L"Свой стиль", RF(area.left + 40, area.top + 2, 200, 26), DWRITE_TEXT_ALIGNMENT_LEADING, kWhite);
        Text(g_fSmall, L"Меняй — превью и сам остров наверху обновляются сразу", RF(area.left + 40, area.top + 26, area.right - area.left - 50, 16),
             DWRITE_TEXT_ALIGNMENT_LEADING, kGray);
        float pw = area.right - area.left - 10;
        D2D1_RECT_F base = RF(area.left, area.top + 48, pw / 2, 46);  // рисуем в 2 раза крупнее
        D2D1_MATRIX_3X2_F t0;
        g_rt->GetTransform(&t0);
        g_rt->SetTransform(D2D1::Matrix3x2F::Scale(2.f, 2.f, Pt(base.left, base.top)) * t0);
        DrawStylePreview(ST_CUSTOM, base, false, false);
        g_rt->SetTransform(t0);
        area.top += 48 + 92 + 10;
    }
    g_setClip = area;
    AddHit(area, H_SSCROLL);
    g_rt->PushAxisAlignedClip(&area, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    SRow r = {area.left, area.right - area.left - 10, area.top};
    g_rcCity = g_rcIcs = D2D1_RECT_F{};
    wchar_t b[96];
    switch (g_setSec) {
        case 0: {  // Главное — то, что реально нужно
            SToggle(r, L"Открывать при наведении", L"Выключено — остров открывается кликом", g_set.hoverOpen);
            g_autostartUi = IsAutostart();
            SToggle(r, L"Запускать вместе с Windows", L"", g_autostartUi, S_AUTOSTART);
            SToggle(r, L"Не беспокоить", L"Без уведомлений и всплывающих событий", g_set.dnd);
            swprintf(b, 96, L"%d%%", g_set.sizePct);
            SSlider(r, L"Размер острова", g_set.sizePct, 80, 200, 5, b);
            SChoice(r, L"Рядом с часами", g_set.idleWidget, {L"Погода", L"Скорость сети", L"Процессор и видеокарта"});
            SInput(r, L"Город для погоды", L"Пусто — определю сам. Например: Иркутск", g_rcCity, IE_CITY);
            SToggle(r, L"Прятать за край экрана", L"Выезжает, когда мышь у верхнего края", g_set.autoHide);
            if (g_set.autoHide) SToggle(r, L"…но не прятать, когда играет музыка", L"", g_set.autoHideMusic);
            {
                std::vector<std::wstring> o = {L"Основной", L"Где мышь"};
                for (size_t i = 0; i < g_monList.size(); i++) o.push_back(L"Монитор " + std::to_wstring(i + 1));
                if (g_monList.size() > 1) SChoice(r, L"На каком мониторе", g_monSel, o, S_MONITOR);
            }
            break;
        }
        case 1: {  // Вид
            if (!g_custOpen) {
                float y = r.y - g_setScroll;
                if (RowVisible(y, 24)) Text(g_fClip, L"Стиль острова", RF(r.x + 12, y + 2, r.w - 24, 22), DWRITE_TEXT_ALIGNMENT_LEADING, kWhite);
                r.y += 30;
                float gap = 10, cw = (r.w - 24 - 2 * gap) / 3, ch = 64;
                for (int k = 0; k < 9; k++) {
                    int i = kStyleShown[k];
                    float cx = r.x + 12 + (k % 3) * (cw + gap), cy = r.y + (k / 3) * (ch + 44) - g_setScroll;
                    if (!RowVisible(cy, ch + 40)) continue;
                    D2D1_RECT_F pr = RF(cx, cy, cw, ch);
                    DrawStylePreview(i, pr, g_set.style == i, Hot(H_SSTYLE, i));
                    if (i == ST_CUSTOM) {  // значок «настроить»
                        Circle(pr.right - 18, pr.bottom - 18, 12, Col(0, 0, 0, 0.45f));
                        IconGear(pr.right - 18, pr.bottom - 18, Col(1, 1, 1));
                    }
                    Text(g_fToast, kStyleNames[i], RF(cx + 2, cy + ch + 4, cw, 18), DWRITE_TEXT_ALIGNMENT_LEADING, kWhite);
                    Text(g_fSmall, i == ST_CUSTOM ? L"Настрой всё сам ›" : kStyleSubs[i], RF(cx + 2, cy + ch + 22, cw, 16), DWRITE_TEXT_ALIGNMENT_LEADING,
                         i == ST_CUSTOM ? g_accent : kGray);
                    AddHit(RF(cx, cy, cw, ch + 40), H_SSTYLE, i);
                }
                r.y += 3 * (ch + 44) + 6;
                SChoice(r, L"Тема", g_set.theme, {L"Тёмная", L"Светлая", L"Как в Windows"}, S_THEME);
                SChoice(r, L"Цвет акцента", g_set.accentMode, {L"Из обложки", L"Системный", L"Свой"}, S_THEME);
                if (g_set.accentMode == 2) {
                    float y2 = r.y - g_setScroll;
                    if (RowVisible(y2, 44))
                        for (int i = 0; i < 8; i++) {
                            float cx = r.x + 30 + i * 40, cy = y2 + 20;
                            AddHit(RF(cx - 17, cy - 17, 34, 34), H_SSWATCH, i);
                            if (g_set.accentIdx == i) StrokeArc(cx, cy, 16, 0.f, 6.2832f, 2.f, kWhite);
                            Circle(cx, cy, Hot(H_SSWATCH, i) ? 13.f : 12.f, kSwatch[i]);
                        }
                    r.y += 48;
                }
                SChoice(r, L"Анимации", g_set.anim, {L"Спокойные", L"Живые", L"Пружинистые"});
                SToggle(r, L"Двигается под музыку", L"Волна и пульс в такт", g_set.audioAnim);
                if (g_set.style == ST_GLASS && !g_glassAllowed)
                    SInfo(r, L"Для настоящего стекла нужна Windows 10 2004 или новее — сейчас стекло матовое.", 18);
            } else {  // конструктор
                SChoice(r, L"Форма", g_set.cShape, {L"Чёлка у края", L"Капсула", L"Пиксели"}, S_THEME);
                SChoice(r, L"Фон", g_set.cBg, {L"Тёмный", L"Стекло", L"Аврора", L"Material", L"Mica", L"Чёрный"}, S_THEME);
                SChoice(r, L"Рамка", g_set.cBorder, {L"Нет", L"Тонкая", L"Неон", L"Блик", L"Пиксель", L"Терминал"}, S_THEME);
                wchar_t rb[32];
                swprintf(rb, 32, g_set.cRadius >= 100 ? L"круглые" : L"%d%%", g_set.cRadius);
                SSlider(r, L"Скругление", g_set.cRadius, 0, 100, 5, rb, S_THEME);
                if (g_set.cBorder) {
                    swprintf(rb, 32, L"%d px", g_set.cBorderW);
                    SSlider(r, L"Толщина рамки", g_set.cBorderW, 1, 4, 1, rb, S_THEME);
                }
                swprintf(b, 96, L"%d%%", g_set.opacity);
                SSlider(r, L"Непрозрачность", g_set.opacity, 60, 100, 1, b, S_THEME);
                SChoice(r, L"Шрифт", g_set.font, {L"Segoe UI", L"Segoe Variable", L"Bahnschrift", L"Consolas"}, S_THEME);
                SToggle(r, L"Тень", L"Остров «парит» над экраном", g_set.cShadow, S_THEME);
                SToggle(r, L"Частицы по сезону", L"Снег, листья, лепестки или светлячки", g_set.cSeason, S_THEME);
                SButton(r, L"Поделиться стилем", L"Сохранить в файл", L"Экспорт", S_EXPORT);
                SButton(r, L"Загрузить стиль", L"Из файла, который прислали", L"Импорт", S_IMPORT);
            }
            break;
        }
        case 2: {  // Питомец — галерея с живыми питомцами
            float gap = 10, cw = (r.w - 24 - 2 * gap) / 3, ch = 112;
            for (int i = 0; i < kCritterCount; i++) {
                float cx = r.x + 12 + (i % 3) * (cw + gap), cy = r.y + (i / 3) * (ch + gap) - g_setScroll;
                if (!RowVisible(cy, ch)) continue;
                D2D1_RECT_F card = RF(cx, cy, cw, ch);
                bool sel = g_set.critter == i, hot = Hot(H_SPET, i);
                AddHit(card, H_SPET, i);
                RRect(card, 16, sel ? WithA(g_accent, 0.16f) : hot ? kCardHot : kCard);
                if (sel) StrokeRRect(card, 16, 2.f, g_accent);
                float px = cx + cw / 2, py = cy + 40;
                if (i == 0) {
                    Ring(px, py, 17, 1.6f, Ov(0.3f));
                    Line(px - 11, py + 11, px + 11, py - 11, 1.6f, Ov(0.3f));
                } else {
                    float hop = hot ? -std::fabs(std::sin((float)NowSec() * 6.f)) * 6.f : 0.f;  // наведи — попрыгает
                    D2D1_MATRIX_3X2_F t0;
                    g_rt->GetTransform(&t0);
                    g_rt->SetTransform(D2D1::Matrix3x2F::Translation(0, hop) * t0);
                    DrawCritter(i, px, py, 46);
                    g_rt->SetTransform(t0);
                    if (hot) g_dirty = true;
                }
                Text(g_fToast, kCritters[i], RF(cx + 6, cy + ch - 44, cw - 12, 20), DWRITE_TEXT_ALIGNMENT_CENTER, kWhite);
                Text(g_fSmall, kCritterSubs[i], RF(cx + 6, cy + ch - 24, cw - 12, 18), DWRITE_TEXT_ALIGNMENT_CENTER, kGray);
            }
            r.y += ((kCritterCount + 2) / 3) * (ch + gap) + 6;
            SToggle(r, L"Живой характер", L"Радуется музыке, обижается на паузу, прыгает и оглядывается", g_set.petReact);
            SToggle(r, L"Засыпает, когда тебя нет", L"Через пару минут без мыши и клавиатуры, ночью — быстрее", g_set.petSleep);
            SInfo(r, L"Питомец живёт в острове рядом с часами и танцует, когда играет музыка.", 20);
            break;
        }
        case 3: {  // Музыка
            SToggle(r, L"Показывать новый трек", L"Остров на 3 секунды раскрывается при смене песни", g_set.peek);
            SToggle(r, L"Текст песен", L"Строки идут в такт музыке, если песня есть в базе", g_set.lyrics);
            if (g_set.lyrics) {
                swprintf(b, 96, L"%+.1f с", g_lyrOffUi / 10.0);
                SSlider(r, L"Текст спешит или опаздывает? Подвинь", g_lyrOffUi, -20, 20, 1, b);
            }
            SChoice(r, L"Прятать плеер на паузе", g_set.pauseHide, {L"Никогда", L"Сразу", L"Через 1 мин", L"Через 5 мин", L"Через 15 мин"}, S_MEDIA);
            SToggle(r, L"Видео из браузера не показывать", L"YouTube-ролики — нет, музыка в браузере — да", g_set.browserMusicOnly, S_MEDIA);
            SToggle(r, L"Громкость в острове", L"Вместо системного окошка Windows", g_set.volHook, S_HOOKS);
            g_srcN = 0;
            {
                size_t a2 = 0;
                const std::wstring& ms = g_set.mediaSeen;
                while (a2 < ms.size() && g_srcN < 24) {
                    size_t e = ms.find(L';', a2);
                    if (e == std::wstring::npos) e = ms.size();
                    std::wstring item = ms.substr(a2, e - a2);
                    a2 = e + 1;
                    size_t eq = item.find(L'=');
                    if (eq == std::wstring::npos) continue;
                    g_srcIds[g_srcN] = item.substr(0, eq);
                    g_srcNames[g_srcN] = item.substr(eq + 1);
                    g_srcOn[g_srcN] = g_set.mediaBlock.find(L"|" + g_srcIds[g_srcN] + L"|") == std::wstring::npos;
                    g_srcN++;
                }
            }
            if (g_srcN) {
                SHeader(r, L"ОТКУДА ПОКАЗЫВАТЬ МУЗЫКУ");
                for (int i = 0; i < g_srcN; i++) SToggle(r, g_srcNames[i], L"", g_srcOn[i], S_MEDIASRC);
            }
            break;
        }
        case 4:  // Уведомления
            SToggle(r, L"Уведомления Windows в острове", L"Сообщения из Telegram, Discord и других", g_set.notifs, S_HOOKS);
            SToggle(r, L"Прятать текст сообщений", L"Видно только, от кого — для стримов и чужих глаз", g_set.notifPrivate);
            SToggle(r, L"Звонки", L"Кнопка микрофона во время звонка", g_set.calls, S_HOOKS);
            SToggle(r, L"Тишина во время звонков", L"Discord, Telegram, Zoom…", g_set.dndCalls);
            SToggle(r, L"Наушники, зарядка, Caps Lock", L"Короткие подсказки о событиях системы", g_set.sysToasts, S_HOOKS);
            SToggle(r, L"Точка микрофона и камеры", L"Оранжевая и зелёная, как на iPhone", g_set.privacy);
            SToggle(r, L"VPN", L"Подсказка, когда VPN включается или выключается", g_set.vpn, S_VPN);
            SToggle(r, L"Скоро дождь", L"Предупреждение за час-два", g_set.rainAlert);
            break;
        case 5:  // Игры
            SChoice(r, L"Когда игра на весь экран", g_set.gameHud, {L"Прятать остров", L"Показывать FPS"}, S_GAME);
            if (g_set.gameHud == 1) {
                SToggle(r, L"Считать FPS", L"Нужны права администратора, иначе — только нагрузка", g_set.gameFps, S_GAME);
                if (g_set.gameFps) {
                    g_adminTaskUi = SchedTaskOn();
                    SToggle(r, L"FPS сразу после включения ПК", L"Остров стартует с правами администратора (Windows спросит один раз)",
                            g_adminTaskUi, S_ADMINTASK);
                    if (!IsElevated())
                        SButton(r, L"Права администратора сейчас", L"Перезапустить остров, чтобы FPS считался уже в этот раз", L"Перезапустить", S_ADMIN);
                }
            }
            SToggle(r, L"Итоги после игры", L"Сколько играл и средний FPS", g_set.gameSummary);
            if (!g_fsExe.empty())
                SInfo(r, (g_game.on ? L"Сейчас: " : L"Последняя на весь экран: ") + g_fsExe +
                             (g_game.on ? L" — игра" : g_fsVideo ? L" — видео/браузер" : g_notGame.count(g_fsPid) ? L" — не игра" : L" — проверяю…"), 20);
            SInfo(r, L"В эксклюзивном полноэкранном режиме Windows не пускает окна поверх игры — там будут только итоги.", 34);
            break;
        case 6: {  // Инструменты
            SToggle(r, L"Поиск по Alt+Space", HotkeyCmdText(), g_set.cmd, S_HOOKS);
            if (g_set.cmd) SChoice(r, L"Искать в интернете через", g_set.search, {L"Google", L"Яндекс"}, S_APPLY);
            SToggle(r, L"Скриншоты и запись", L"Ctrl+Alt+S снимок · T текст с экрана · R запись · C пипетка", g_set.tools, S_HOOKS);
            if (g_set.tools) SToggle(r, L"Редактор после снимка", L"Стрелки, рамки, текст, размытие", g_set.markup);
            SToggle(r, L"История буфера обмена", L"Ctrl+Alt+V — всё, что копировал", g_set.clip, S_HOOKS);
            if (g_set.clip) {
                swprintf(b, 96, L"%d записей", g_clipLimUi * 50);
                SSlider(r, L"Сколько хранить", g_clipLimUi, 1, 20, 1, b, S_CLIPLIMIT);
                SButton(r, L"Очистить историю", L"Закреплённые останутся", L"Очистить", S_CLIPCLEAR, true);
            }
            SToggle(r, L"Загрузки из браузера", L"Прогресс и готовый файл в острове", g_set.downloads, S_HOOKS);
            break;
        }
        case 7:  // Календарь
            SToggle(r, L"Погода и календарь в острове", L"", g_set.widgets, S_HOOKS);
            SInput(r, L"Ссылка на календарь (ICS)", L"Google: настройки календаря → «Секретный адрес в формате iCal»", g_rcIcs, IE_ICS);
            SToggle(r, L"Напоминать о событиях", L"За 10 минут до начала", g_set.calRemind);
            break;
        case 8: {  // Дополнительно — для тех, кто хочет покрутить
            SHeader(r, L"ОСТРОВ");
            if (g_set.topOffset == 0) swprintf(b, 96, L"0 — прижат к краю");
            else swprintf(b, 96, L"%d px", g_set.topOffset);
            SSlider(r, L"Отступ сверху", g_set.topOffset, 0, 40, 1, b);
            SToggle(r, L"Клики сквозь остров", L"Свёрнутый остров не мешает нажимать то, что под ним", g_set.passThrough, S_PASS);
            SInput(r, L"Прятать в программах", L"Имена exe через запятую, например: obs64, vlc", g_rcCity, IE_EXCLUDE);
            SHeader(r, L"ВКЛАДКИ");
            DrawTabsEditor(r);
            SHeader(r, L"ЭФФЕКТЫ");
            SToggle(r, L"Прогресс по краю", L"Загрузки бегут полоской по контуру", g_set.fxOutline, S_FX);
            SToggle(r, L"Две активности сразу", L"Рядом отделяется маленький «пузырь»", g_set.fxSplit, S_FX);
            SToggle(r, L"Перекатывающиеся цифры", L"Часы меняются как в iOS", g_set.fxRoll, S_FX);
            SToggle(r, L"Потянуть вниз", L"Схвати остров и потяни — раскроется", g_set.fxPull, S_FX);
            SToggle(r, L"Частицы", L"Конфетти, искры, салют", g_set.fxParticles, S_FX);
            SToggle(r, L"Звуки", L"Тихие щелчки интерфейса", g_set.fxSounds, S_FX);
            SToggle(r, L"Новогодний отсчёт", L"С 20 декабря", g_set.fxNY, S_FX);
            break;
        }
        case 9: {  // О программе
            {
                float y = r.y - g_setScroll;
                if (RowVisible(y, 84)) {
                    DrawLogo(r.x + 12, y + 6, 68);
                    Text(g_fPlayerTitle, kAppName, RF(r.x + 96, y + 12, r.w - 110, 28), DWRITE_TEXT_ALIGNMENT_LEADING, kWhite);
                    Text(g_fClip, std::wstring(L"Версия ") + kAppVer + L" · остров для Windows", RF(r.x + 96, y + 42, r.w - 110, 20),
                         DWRITE_TEXT_ALIGNMENT_LEADING, kGray);
                }
                r.y += 90;
            }
            SInfo(r, L"Клик по острову — открыть · Alt+Space — поиск · Ctrl+Alt+V — буфер", 20);
            SInfo(r, L"Ctrl+Alt+S / T / C — снимок, текст с экрана, пипетка · запись — своя клавиша во вкладке «Запись»", 34);
            SHeader(r, L"ОБНОВЛЕНИЯ");
            SToggle(r, L"Проверять обновления", L"Раз в несколько часов; ставится только по твоему клику", g_set.autoUpdate);
            SButton(r, L"Проверить сейчас", g_updStatus.empty() ? std::wstring(L"Сейчас версия ") + kAppVer : g_updStatus,
                    g_upd.url.empty() || g_updBusy ? L"Проверить" : L"Обновить", S_UPDCHECK);
            SHeader(r, L"НАСТРОЙКИ И ДАННЫЕ");
            SButton(r, L"Сохранить настройки в файл", L"Чтобы перенести на другой ПК или не потерять", L"Сохранить", S_BACKUP);
            SButton(r, L"Загрузить настройки из файла", L"Остров перезапустится с ними", L"Загрузить", S_RESTORE);
            SButton(r, L"Папка с данными", L"Скриншоты, история буфера, журнал ошибок", L"Открыть", S_OPENDATA);
            SButton(r, L"Журнал ошибок", L"Если что-то сломалось — пришли этот файл", L"Показать", S_LOGS);
            SButton(r, L"Показать знакомство ещё раз", L"5 подсказок о том, что умеет остров", L"Показать", S_WELCOME);
            SButton(r, L"Сбросить все настройки", L"Как при первом запуске", L"Сбросить", S_RESET, true);
            break;
        }
    }
    g_rt->PopAxisAlignedClip();
    g_setContentH = r.y - area.top;
    float maxS = std::max(0.f, g_setContentH - (area.bottom - area.top));
    if (g_setScrollT > maxS) g_setScrollT = maxS;
    if (g_setScroll > maxS + 0.5f) g_setScroll = maxS;
    // полоса прокрутки — её можно тащить мышью
    if (maxS > 0) {
        float vh = area.bottom - area.top, th = std::max(30.f, vh * vh / g_setContentH);
        float ty = area.top + (vh - th) * (g_setScroll / maxS);
        bool hot = Hot(H_SBAR) || g_sbarDrag;
        RRect(RF(area.right - (hot ? 7.f : 5.f), ty, hot ? 6.f : 4.f, th), 3, Ov(hot ? 0.38f : 0.2f));
        AddHit(RF(area.right - 14, area.top, 16, vh), H_SBAR);
    }
}

// ---- поведение ----
static std::wstring HotkeyCmdText() {
    if (!g_set.cmd) return L"Поиск программ и файлов, калькулятор, курсы, команды";
    return std::wstring(g_cmdHotkeyText.empty() ? L"Горячая клавиша занята другой программой" : g_cmdHotkeyText) +
           L" — программы, файлы, расчёты, команды";
}
static void SettingsOpened() {
    g_monList = ListMonitors();
    g_monSel = 0;
    if (g_set.followMouse) g_monSel = 1;
    else if (!g_set.monitor.empty())
        for (size_t i = 0; i < g_monList.size(); i++)
            if (g_monList[i].device == g_set.monitor) g_monSel = (int)i + 2;
    g_lyrOffUi = g_set.lyricOffsetMs / 100;
    g_clipLimUi = std::max(1, g_set.clipLimit / 50);
    g_setArm = -1;
    if (g_ieFocus != IE_CITY) g_ie[IE_CITY].text = g_set.city;
    if (g_ieFocus != IE_ICS) g_ie[IE_ICS].text = g_set.icsUrl;
    if (g_ieFocus != IE_EXCLUDE) g_ie[IE_EXCLUDE].text = g_set.exclude;
}
// ---- стиль в файл и обратно (чтобы делиться) ----
static void ExportStyle() {
    wchar_t buf[MAX_PATH] = L"Мой стиль.island-style";
    OPENFILENAMEW of = {};
    of.lStructSize = sizeof(of);
    of.lpstrFilter = L"Стиль острова\0*.island-style\0";
    of.lpstrFile = buf;
    of.nMaxFile = MAX_PATH;
    of.lpstrDefExt = L"island-style";
    of.Flags = OFN_OVERWRITEPROMPT | OFN_EXPLORER;
    if (!GetSaveFileNameW(&of)) return;
    wchar_t t[512];
    swprintf(t, 512,
             L"# Zen Island style\nshape=%d\nbg=%d\nborder=%d\nradius=%d\nborderw=%d\nshadow=%d\nseason=%d\ntop=%d\nfont=%d\n"
             L"theme=%d\naccentmode=%d\naccent=%d\nopacity=%d\nanim=%d\n",
             g_set.cShape, g_set.cBg, g_set.cBorder, g_set.cRadius, g_set.cBorderW, (int)g_set.cShadow, (int)g_set.cSeason,
             g_set.topOffset, g_set.font, g_set.theme, g_set.accentMode, g_set.accentIdx, g_set.opacity, g_set.anim);
    WriteFileU8(buf, WToU8(t));
    ShowActionToast(TK_TEXT, L"Стиль сохранён", L"Показать", TA_SELECT, buf, g_accent, 2500);
}
static void ImportStyle() {
    wchar_t buf[MAX_PATH] = {};
    OPENFILENAMEW of = {};
    of.lStructSize = sizeof(of);
    of.lpstrFilter = L"Стиль острова\0*.island-style\0Все файлы\0*.*\0";
    of.lpstrFile = buf;
    of.nMaxFile = MAX_PATH;
    of.Flags = OFN_FILEMUSTEXIST | OFN_EXPLORER;
    if (!GetOpenFileNameW(&of)) return;
    std::wstring all = U8ToW(ReadFileU8(buf));
    int got = 0;
    size_t a = 0;
    while (a < all.size()) {
        size_t b = all.find(L'\n', a);
        if (b == std::wstring::npos) b = all.size();
        std::wstring line = all.substr(a, b - a);
        a = b + 1;
        size_t eq = line.find(L'=');
        if (eq == std::wstring::npos || line[0] == L'#') continue;
        std::wstring k = line.substr(0, eq);
        int v = _wtoi(line.c_str() + eq + 1);
        got++;
        if (k == L"shape") g_set.cShape = Clamp(v, 0, 2);
        else if (k == L"bg") g_set.cBg = Clamp(v, 0, 5);
        else if (k == L"border") g_set.cBorder = Clamp(v, 0, 5);
        else if (k == L"radius") g_set.cRadius = Clamp(v, 0, 100);
        else if (k == L"borderw") g_set.cBorderW = Clamp(v, 1, 4);
        else if (k == L"shadow") g_set.cShadow = v != 0;
        else if (k == L"season") g_set.cSeason = v != 0;
        else if (k == L"top") g_set.topOffset = Clamp(v, 0, 40);
        else if (k == L"font") g_set.font = Clamp(v, 0, 3);
        else if (k == L"theme") g_set.theme = Clamp(v, 0, 2);
        else if (k == L"accentmode") g_set.accentMode = Clamp(v, 0, 2);
        else if (k == L"accent") g_set.accentIdx = Clamp(v, 0, 7);
        else if (k == L"opacity") g_set.opacity = Clamp(v, 60, 100);
        else if (k == L"anim") g_set.anim = Clamp(v, 0, 2);
        else got--;
    }
    if (!got) { Shake(); return; }
    g_set.style = ST_CUSTOM;
    ApplySettings();
    ApplyTheme();
    StyleMorphKick();
    ShowActionToast(TK_TEXT, L"Стиль загружен", L"", TA_NONE, L"", g_accent, 2000);
}
static void SettingDone(int special) {
    switch (special) {
        case S_THEME: SaveSettings(); ApplyTheme(); break;
        case S_PASS: SaveSettings(); UpdatePassThrough(); break;
        case S_GAME:
            SaveSettings();
            if (g_game.on) GameOff();
            g_notGame.clear();
            CheckFullscreen();
            break;
        case S_ADMIN: RestartElevated(); break;
        case S_UPDCHECK: if (!g_upd.url.empty() && !g_updBusy) UpdateStartDownload(); else UpdateCheck(true); break;
        case S_WELCOME: g_welcomeStep = 0; g_launchWelcome = true; SetMode(Collapsed()); SetTimer(g_hwnd, T_WELCOME, 600, nullptr); break;
        case S_BACKUP: SettingsBackup(); break;
        case S_RESTORE: SettingsRestore(); break;
        case S_LOGS: OpenLogFile(); break;
        case S_ADMINTASK:
            if (g_adminTaskUi) {
                if (SchedTaskCreate()) {
                    SchedTaskMark(true);
                    SetAutostart(true);  // обычный автозапуск больше не нужен
                    ShowActionToast(TK_TEXT, L"Готово: FPS будет с самого старта", L"", TA_NONE, L"", g_accent, 3000);
                } else ShowActionToast(TK_ERROR, L"Windows не разрешила — попробуй ещё раз", L"", TA_NONE, L"", kWhite, 3000);
            } else {
                SchedTaskRemove();
                SetAutostart(true);
            }
            break;
        case S_MEDIA: SaveSettings(); MediaFilterReset(); OnMedia(); break;
        case S_MEDIASRC: {
            std::wstring blk = L"|";
            for (int i = 0; i < g_srcN; i++) if (!g_srcOn[i]) blk += g_srcIds[i] + L"|";
            g_set.mediaBlock = blk.size() > 1 ? blk : L"";
            SaveSettings();
            OnMedia();
            break;
        }
        case S_VPN:
            SaveSettings();
            if (g_set.vpn) { VpnWatchStart(g_hwnd); g_vpnForceToast = true; VpnCheckAsync(g_hwnd, true); }
            else VpnWatchStop();
            break;
        case S_FX: SaveSettings(); if (FxBusy()) FxEnsureTimer(); break;
        case S_FX + 100: Confetti(); Fireworks(4); PlayUi(SN_DONE); break;
        case S_EXPORT: ExportStyle(); break;
        case S_IMPORT: ImportStyle(); break;
        case S_AUTOSTART: SetAutostart(g_autostartUi); break;
        case S_MONITOR:
            g_set.followMouse = g_monSel == 1;
            g_set.monitor = g_monSel >= 2 && g_monSel - 2 < (int)g_monList.size() ? g_monList[g_monSel - 2].device : L"";
            ApplySettings();
            break;
        case S_CLIPLIMIT:
            g_set.clipLimit = g_clipLimUi * 50;
            if (g_clipStoreOk) g_clip.SetLimit(g_set.clipLimit);
            SaveSettings();
            break;
        case S_CLIPCLEAR: if (g_clipStoreOk) g_clip.Clear(); break;
        case S_OPENDATA: ShellExecuteW(nullptr, L"open", (g_baseDir + L"\\data").c_str(), nullptr, nullptr, SW_SHOWNORMAL); break;
        case S_RESET: {
            g_set = Settings{};
            SetAutostart(false);
            SettingsOpened();
            ApplySettings();
            ApplyTheme();
            break;
        }
        default:
            g_set.lyricOffsetMs = g_lyrOffUi * 100;
            ApplySettings();
    }
    g_dirty = true;
}
static void SliderSet(int idx, int x) {
    if (idx < 0 || idx >= (int)g_sctl.size()) return;
    SCtl& c = g_sctl[idx];
    float fx = x / g_scale;
    float f = Clamp((fx - c.track.left) / std::max(1.f, c.track.right - c.track.left), 0.f, 1.f);
    int v = c.mn + (int)std::lround(f * (c.mx - c.mn) / c.step) * c.step;
    v = Clamp(v, c.mn, c.mx);
    if (*c.i != v) {
        *c.i = v;
        if (c.i == &g_lyrOffUi) g_set.lyricOffsetMs = v * 100;
        g_dirty = true;
    }
}
// клики по настройкам; true — обработано
static bool SettingsClick(const Hit& h, int x) {
    (void)x;
    switch (h.id) {
        case H_SNAV: g_setSec = h.arg; g_setScroll = g_setScrollT = 0; g_setArm = -1; IEBlur(true); g_dirty = true; return true;
        case H_SCTL: {
            if (h.arg >= (int)g_sctl.size()) return true;
            SCtl& c = g_sctl[h.arg];
            if (c.kind == 0 && c.b) { *c.b = !*c.b; SettingDone(c.special); }
            else if (c.kind == 3) {
                bool danger = c.special == S_CLIPCLEAR || c.special == S_RESET;
                if (danger && !(g_setArm == h.arg && NowSec() < g_setArmUntil)) {
                    g_setArm = h.arg;
                    g_setArmUntil = NowSec() + 3;
                    g_dirty = true;
                    return true;
                }
                g_setArm = -1;
                SettingDone(c.special);
            }
            return true;
        }
        case H_SCHOICE: {
            int idx = h.arg / 32, opt = h.arg % 32;
            if (idx < (int)g_sctl.size() && g_sctl[idx].i) {
                *g_sctl[idx].i = opt;
                SettingDone(g_sctl[idx].special);
            }
            return true;
        }
        case H_SSWATCH: g_set.accentIdx = h.arg; SettingDone(S_THEME); return true;
        case H_SSTYLE:
            ApplyStyle(h.arg);
            if (h.arg == ST_CUSTOM) { g_custOpen = true; g_setScroll = g_setScrollT = 0; }
            SettingsOpened();
            g_dirty = true;
            return true;
        case H_SCUST: g_custOpen = h.arg == 0; g_setScroll = g_setScrollT = 0; g_dirty = true; return true;
        case H_SPET:
            g_set.critter = h.arg;
            SaveSettings();
            if (h.arg > 0) { g_act = ACT_HOP; g_actT0 = NowSec(); g_actUntil = g_actT0 + 0.9; }  // радостно подпрыгнул
            g_dirty = true;
            return true;
        case H_STABVIS: {
            int mask = g_set.tabHidden ^ (1 << h.arg);
            if (mask != 0x7F) g_set.tabHidden = mask;  // хотя бы одна вкладка остаётся
            if (g_set.tabHidden & (1 << g_tab)) {}
            SaveSettings();
            g_dirty = true;
            return true;
        }
        case H_STABUP:
        case H_STABDN: {
            int a = h.arg, b = h.id == H_STABUP ? a - 1 : a + 1;
            if (b >= 0 && b < 7) std::swap(g_set.tabOrder[a], g_set.tabOrder[b]);
            SaveSettings();
            g_dirty = true;
            return true;
        }
    }
    return false;
}
static bool SettingsDown(const Hit& h, int x) {
    if (h.id == H_SSLIDER) {
        g_setDrag = h.arg;
        SetCapture(g_hwnd);
        SliderSet(h.arg, x);
        return true;
    }
    return false;
}
static void SettingsDragEnd() {
    if (g_setDrag < 0) return;
    int idx = g_setDrag;
    g_setDrag = -1;
    ReleaseCapture();
    if (idx < (int)g_sctl.size()) SettingDone(g_sctl[idx].special);
}
static void SettingsTextCommit() {
    bool ch = false;
    auto trim = [](std::wstring t) { while (!t.empty() && t.back() == L' ') t.pop_back(); while (!t.empty() && t[0] == L' ') t.erase(0, 1); return t; };
    { std::wstring t = trim(g_ie[IE_CITY].text); if (t != g_set.city) { g_set.city = t; ch = true; } }
    { std::wstring t = trim(g_ie[IE_ICS].text); if (t != g_set.icsUrl) { g_set.icsUrl = t; ch = true; } }
    { std::wstring t = trim(g_ie[IE_EXCLUDE].text); if (t != g_set.exclude) { g_set.exclude = t; ch = true; } }
    if (ch) ApplySettings();
}
