// apppick.inc — выбор программ для дока на «Главной»: поиск по названию вместо окна «Открыть файл»
// (подключается в island.cpp после cmd.inc — пользуется его списком программ и кэшем иконок)

static std::vector<int> g_appMatch;  // индексы в g_index->apps, что сейчас на экране (по порядку)

static void AppPickOpen() {
    g_appPick = true;
    g_appScroll = 0;
    g_ie[IE_APPS].text.clear();
    CmdRequestIndex(false);
    IEFocus(IE_APPS);
    g_dirty = true;
}
static void AppPickClose() {
    g_appPick = false;
    if (g_ieFocus == IE_APPS) IEBlur(false);
    g_dirty = true;
}
static void AppPickToggle(int idx) {
    if (!g_index || idx < 0 || idx >= (int)g_appMatch.size()) { Shake(); return; }
    const CmdEntry& e = g_index->apps[g_appMatch[idx]];
    int have = LauncherFind(e.target);
    if (have >= 0) LauncherRemove(have);
    else {
        LauncherAddPath(e.target, true, e.name);
        PlayUi(SN_DONE);
    }
    g_dirty = true;
}
static void AppPickFilter() {
    g_appMatch.clear();
    if (!g_index) return;
    std::wstring q = LowerW(g_ie[IE_APPS].text);
    while (!q.empty() && q.back() == L' ') q.pop_back();
    const auto& apps = g_index->apps;
    std::vector<std::pair<int, int>> sc;  // (оценка, индекс): начало имени > начало слова > где угодно
    for (int i = 0; i < (int)apps.size(); i++) {
        const std::wstring& l = apps[i].lower;
        int s = 0;
        if (q.empty()) s = 1;
        else {
            size_t p = l.find(q);
            if (p == 0) s = 3;
            else if (p != std::wstring::npos) s = (l[p - 1] == L' ' || l[p - 1] == L'-' || l[p - 1] == L'(') ? 2 : 1;
        }
        if (s) sc.push_back({s, i});
    }
    std::stable_sort(sc.begin(), sc.end(), [&](const std::pair<int, int>& a, const std::pair<int, int>& b) {
        if (a.first != b.first) return a.first > b.first;
        return apps[a.second].lower < apps[b.second].lower;
    });
    for (auto& p : sc) g_appMatch.push_back(p.second);
}

static void DrawAppPicker(D2D1_RECT_F c) {
    float cw = c.right - c.left;
    // верхняя строка: назад · поиск · обзор
    D2D1_RECT_F back = RF(c.left, c.top, 44, 44);
    AddHit(back, H_APPCLOSE);
    Circle(c.left + 22, c.top + 22, 22, Hot(H_APPCLOSE) ? Ov(0.14f) : kCard);
    Line(c.left + 25, c.top + 15, c.left + 18, c.top + 22, 2.f, kWhite);
    Line(c.left + 18, c.top + 22, c.left + 25, c.top + 29, 2.f, kWhite);
    const float bw = 116;
    D2D1_RECT_F box = RF(c.left + 54, c.top, cw - 54 - bw - 10, 44);
    DrawIEdit(IE_APPS, box, L"Название программы — например, Telegram", 14);
    IconSearch(box.right - 20, c.top + 22, WithA(kGray, 0.8f));
    D2D1_RECT_F br = RF(c.right - bw, c.top, bw, 44);
    AddHit(br, H_APPBROWSE);
    RRect(br, 14, Hot(H_APPBROWSE) ? Ov(0.14f) : kCard);
    Text(g_fSmall, L"Обзор…", br, DWRITE_TEXT_ALIGNMENT_CENTER, kWhite);

    AppPickFilter();
    D2D1_RECT_F area = RF(c.left, c.top + 56, cw, c.bottom - c.top - 56);
    if (!g_index || g_appMatch.empty()) {
        RRect(area, 20, kCard);
        std::wstring t = !g_index ? L"Собираю список программ…" : L"Ничего не нашлось — можно выбрать файл через «Обзор…»";
        Text(g_fToast, t, area, DWRITE_TEXT_ALIGNMENT_CENTER, kGray);
        return;
    }
    const int cols = 7;
    const float cellW = cw / cols, cellH = 90;
    int rowsVis = std::max(1, (int)((area.bottom - area.top) / cellH));
    int rowsAll = ((int)g_appMatch.size() + cols - 1) / cols;
    g_appScroll = Clamp(g_appScroll, 0, std::max(0, rowsAll - rowsVis));
    int budget = 6;  // иконки догружаем понемногу, чтобы кадр не тормозил
    for (int k = g_appScroll * cols; k < (int)g_appMatch.size() && k < (g_appScroll + rowsVis) * cols; k++) {
        int vis = k - g_appScroll * cols;
        const CmdEntry& e = g_index->apps[g_appMatch[k]];
        D2D1_RECT_F r = RF(area.left + (vis % cols) * cellW + 3, area.top + (vis / cols) * cellH, cellW - 6, cellH - 6);
        AddHit(r, H_APPPICK, k);
        bool pinned = LauncherFind(e.target) >= 0;
        bool first = k == 0 && !g_ie[IE_APPS].text.empty();  // её возьмёт Enter
        if (Hot(H_APPPICK, k) || pinned || first) RRect(r, 16, pinned ? WithA(g_accent, 0.16f) : Hot(H_APPPICK, k) ? kCardHot : Ov(0.05f));
        float icx = (r.left + r.right) / 2, icy = r.top + 30;
        if (const CmdIcon* ic = CmdGetIcon(e.target, budget))
            DrawPicRounded(const_cast<Pic&>(ic->pic), RF(icx - 20, icy - 20, 40, 40), 8, 1.f);
        else {
            RRect(RF(icx - 20, icy - 20, 40, 40), 10, Ov(0.1f));
            Text(g_fToast, e.name.substr(0, 1), RF(icx - 20, icy - 20, 40, 40), DWRITE_TEXT_ALIGNMENT_CENTER, kWhite);
        }
        if (pinned) {
            Circle(icx + 18, icy - 17, 8, g_accent);
            IconCheck(icx + 18, icy - 17, Col(1, 1, 1));
        }
        Text(g_fSmall, FitText(g_fSmall, e.name, cellW - 14), RF(r.left + 4, r.top + 56, r.right - r.left - 8, 20),
             DWRITE_TEXT_ALIGNMENT_CENTER, pinned ? kWhite : WithA(kWhite, 0.85f));
    }
    // полоска прокрутки
    if (rowsAll > rowsVis) {
        float h = area.bottom - area.top, th = std::max(24.f, h * rowsVis / rowsAll);
        float ty = area.top + (h - th) * g_appScroll / (float)(rowsAll - rowsVis);
        RRect(RF(c.right + 8, ty, 3, th), 1.5f, Ov(0.25f));
    }
}
